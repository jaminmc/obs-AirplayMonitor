/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "encode/vt_encoder.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CMFormatDescriptionBridge.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <mach/mach_time.h>
#include <memory>
#include <mutex>

namespace airplay {
namespace {

uint64_t host_ns()
{
	static mach_timebase_info_data_t info = []() {
		mach_timebase_info_data_t value{};
		mach_timebase_info(&value);
		return value;
	}();
	return mach_absolute_time() * info.numer / info.denom;
}

bool set_number(VTCompressionSessionRef session, CFStringRef key, int64_t value)
{
	CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
	OSStatus status = VTSessionSetProperty(session, key, number);
	CFRelease(number);
	return status == noErr;
}

std::vector<uint8_t> image_description(CMVideoFormatDescriptionRef format)
{
	const CMImageDescriptionFlavor flavors[] = {kCMImageDescriptionFlavor_QuickTimeMovie,
						    kCMImageDescriptionFlavor_ISOFamily};
	for (CMImageDescriptionFlavor flavor : flavors) {
		CMBlockBufferRef block = nullptr;
		if (CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer(
			    kCFAllocatorDefault, format, CFStringGetSystemEncoding(), flavor, &block) != noErr ||
		    !block)
			continue;
		size_t length = CMBlockBufferGetDataLength(block);
		std::vector<uint8_t> bytes(length);
		OSStatus copied = CMBlockBufferCopyDataBytes(block, 0, length, bytes.data());
		CFRelease(block);
		if (copied != kCMBlockBufferNoErr || bytes.size() < 16)
			continue;
		/* The bridge leaves dataRefIndex as 0xFFFF. A receiver treats that as an invalid description. */
		bytes[14] = 0;
		bytes[15] = 1;
		uint32_t size = (uint32_t)bytes.size();
		bytes[0] = (uint8_t)(size >> 24);
		bytes[1] = (uint8_t)(size >> 16);
		bytes[2] = (uint8_t)(size >> 8);
		bytes[3] = (uint8_t)size;
		CMVideoFormatDescriptionRef parsed = nullptr;
		OSStatus parsed_status = CMVideoFormatDescriptionCreateFromBigEndianImageDescriptionData(
			kCFAllocatorDefault, bytes.data(), bytes.size(), CFStringGetSystemEncoding(), flavor, &parsed);
		if (parsed_status == noErr && parsed) {
			CFRelease(parsed);
			return bytes;
		}
		if (parsed)
			CFRelease(parsed);
	}
	return {};
}

bool set_bool(VTCompressionSessionRef session, CFStringRef key, bool value)
{
	return VTSessionSetProperty(session, key, value ? kCFBooleanTrue : kCFBooleanFalse) == noErr;
}

struct EncodeMark {
	uint64_t start_ns = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	VtEncoder::Callback callback;
};

void compression_callback(void *, void *source_frame, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
{
	std::unique_ptr<EncodeMark> mark(static_cast<EncodeMark *>(source_frame));
	if (status != noErr || !sample || !mark || !mark->callback)
		return;
	EncodedAu au;
	au.width = mark->width;
	au.height = mark->height;
	au.encode_ns = host_ns() - mark->start_ns;
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
	au.keyframe = true;
	if (attachments && CFArrayGetCount(attachments) > 0) {
		CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
		CFBooleanRef not_sync = (CFBooleanRef)CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
		if (not_sync && CFBooleanGetValue(not_sync))
			au.keyframe = false;
	}
	CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
	if (format && au.keyframe) {
		CFDictionaryRef atoms = (CFDictionaryRef)CMFormatDescriptionGetExtension(
			format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
		if (atoms) {
			CFDataRef config = (CFDataRef)CFDictionaryGetValue(atoms, CFSTR("hvcC"));
			if (!config)
				config = (CFDataRef)CFDictionaryGetValue(atoms, CFSTR("avcC"));
			if (config) {
				auto *bytes = CFDataGetBytePtr(config);
				CFIndex length = CFDataGetLength(config);
				au.codec_config.assign(bytes, bytes + length);
				au.sample_description = image_description(format);
			}
		}
	}
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
	if (!block)
		return;
	size_t length = CMBlockBufferGetDataLength(block);
	au.data.resize(length);
	if (CMBlockBufferCopyDataBytes(block, 0, length, au.data.data()) != kCMBlockBufferNoErr)
		return;
	mark->callback(std::move(au));
}

void copy_plane(uint8_t *dst, uint32_t dst_stride, const uint8_t *src, uint32_t src_stride, uint32_t width, uint32_t rows)
{
	if (dst_stride == width && src_stride == width) {
		std::memcpy(dst, src, (size_t)width * rows);
		return;
	}
	for (uint32_t row = 0; row < rows; row++)
		std::memcpy(dst + (size_t)row * dst_stride, src + (size_t)row * src_stride, width);
}

} /* namespace */

struct VtEncoder::Impl {
	VTCompressionSessionRef session = nullptr;
	EncodeConfig config;
	std::mutex codec_mu;
	std::vector<uint8_t> codec_sent;
};

VtEncoder::VtEncoder() : impl_(new Impl) {}

VtEncoder::~VtEncoder()
{
	stop();
	delete impl_;
}

void VtEncoder::stop()
{
	finish();
}

void VtEncoder::finish()
{
	if (!impl_->session)
		return;
	VTCompressionSessionCompleteFrames(impl_->session, kCMTimeInvalid);
	VTCompressionSessionInvalidate(impl_->session);
	CFRelease(impl_->session);
	impl_->session = nullptr;
}

bool VtEncoder::start(const EncodeConfig &config, std::string &error)
{
	finish();
	impl_->config = config;
	impl_->codec_sent.clear();
	CMVideoCodecType codec = config.codec == Codec::Hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
	CFMutableDictionaryRef source = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
								  &kCFTypeDictionaryValueCallBacks);
	int pixel = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pixel_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixel);
	CFDictionarySetValue(source, kCVPixelBufferPixelFormatTypeKey, pixel_number);
	CFRelease(pixel_number);
	int width = (int)config.width;
	int height = (int)config.height;
	CFNumberRef width_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
	CFNumberRef height_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
	CFDictionarySetValue(source, kCVPixelBufferWidthKey, width_number);
	CFDictionarySetValue(source, kCVPixelBufferHeightKey, height_number);
	CFRelease(width_number);
	CFRelease(height_number);

	VTCompressionSessionRef session = nullptr;
	OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, (int32_t)config.width, (int32_t)config.height,
						     codec, nullptr, source, nullptr, compression_callback, nullptr, &session);
	CFRelease(source);
	if (status != noErr || !session) {
		error = config.codec == Codec::Hevc ? "HEVC encoder did not start" : "H.264 encoder did not start";
		return false;
	}
	set_bool(session, kVTCompressionPropertyKey_RealTime, true);
	set_bool(session, kVTCompressionPropertyKey_AllowFrameReordering, false);
	set_number(session, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
	set_number(session, kVTCompressionPropertyKey_ExpectedFrameRate, config.fps);
	set_number(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, std::max(1u, config.fps * 2));
	set_number(session, kVTCompressionPropertyKey_AverageBitRate, config.bitrate);
	int64_t bytes_per_second = std::max<int64_t>(1, (int64_t)config.bitrate / 8);
	int64_t one_second = 1;
	CFNumberRef rate = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes_per_second);
	CFNumberRef window = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &one_second);
	CFMutableArrayRef limits = CFArrayCreateMutable(kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(limits, rate);
	CFArrayAppendValue(limits, window);
	VTSessionSetProperty(session, kVTCompressionPropertyKey_DataRateLimits, limits);
	CFRelease(limits);
	CFRelease(rate);
	CFRelease(window);
	CFStringRef profile = config.codec == Codec::Hevc ? kVTProfileLevel_HEVC_Main_AutoLevel
							  : kVTProfileLevel_H264_High_AutoLevel;
	VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, profile);
	if (VTCompressionSessionPrepareToEncodeFrames(session) != noErr) {
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
		error = "Encoder prepare failed";
		return false;
	}
	impl_->session = session;
	return true;
}

bool VtEncoder::encode(const RawNv12 &frame, const Callback &callback, std::string &error)
{
	if (!impl_->session) {
		error = "Encoder is not running";
		return false;
	}
	size_t need = (size_t)frame.width * frame.height * 3 / 2;
	if (!frame.data || frame.size < need || frame.width != impl_->config.width || frame.height != impl_->config.height) {
		error = "Frame size does not match the encoder";
		return false;
	}
	CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(impl_->session);
	if (!pool) {
		error = "Encoder has no pixel buffer pool";
		return false;
	}
	CVPixelBufferRef pixel = nullptr;
	if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel) != kCVReturnSuccess || !pixel) {
		error = "Could not allocate an encoder frame";
		return false;
	}
	CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
			      kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
			      kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
			      kCVAttachmentMode_ShouldPropagate);
	CVPixelBufferLockBaseAddress(pixel, 0);
	uint8_t *y_dst = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
	size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
	uint8_t *uv_dst = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 1);
	size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
	const uint8_t *y_src = frame.data;
	const uint8_t *uv_src = frame.data + (size_t)frame.width * frame.height;
	copy_plane(y_dst, (uint32_t)y_stride, y_src, frame.width, frame.width, frame.height);
	copy_plane(uv_dst, (uint32_t)uv_stride, uv_src, frame.width, frame.width, frame.height / 2);
	CVPixelBufferUnlockBaseAddress(pixel, 0);

	auto *mark = new EncodeMark;
	mark->start_ns = host_ns();
	mark->width = frame.width;
	mark->height = frame.height;
	mark->callback = [this, callback](EncodedAu &&au) {
		{
			std::lock_guard<std::mutex> lock(impl_->codec_mu);
			if (!au.codec_config.empty() && au.codec_config == impl_->codec_sent) {
				au.codec_config.clear();
				au.sample_description.clear();
			} else if (!au.codec_config.empty()) {
				impl_->codec_sent = au.codec_config;
			}
		}
		if (callback)
			callback(std::move(au));
	};
	CFMutableDictionaryRef props = nullptr;
	if (frame.force_keyframe) {
		props = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
						  &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
	}
	CMTime pts = CMTimeMake((int64_t)frame.timestamp_ns, 1000000000);
	OSStatus status = VTCompressionSessionEncodeFrame(impl_->session, pixel, pts, kCMTimeInvalid, props, mark, nullptr);
	if (props)
		CFRelease(props);
	CFRelease(pixel);
	if (status != noErr) {
		delete mark;
		error = "Encode failed";
		return false;
	}
	return true;
}

} /* namespace airplay */
