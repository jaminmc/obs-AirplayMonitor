/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "audio/aac_eld.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cstring>
#include <string>
#include <vector>

namespace airplay {
namespace {

struct Feed {
	const float *data = nullptr;
	UInt32 frames = 0;
	bool consumed = false;
};

OSStatus feed_input(AudioConverterRef, UInt32 *packets, AudioBufferList *buffers, AudioStreamPacketDescription **,
		    void *user)
{
	auto *feed = static_cast<Feed *>(user);
	if (!feed || feed->consumed || feed->frames == 0) {
		*packets = 0;
		return noErr;
	}
	buffers->mNumberBuffers = 1;
	buffers->mBuffers[0].mNumberChannels = 2;
	buffers->mBuffers[0].mData = (void *)feed->data;
	buffers->mBuffers[0].mDataByteSize = feed->frames * 2 * sizeof(float);
	*packets = feed->frames;
	feed->consumed = true;
	return noErr;
}

} /* namespace */

struct AacEldEncoder::Impl {
	AudioConverterRef converter = nullptr;
	uint32_t frame_size = 480;
	uint32_t timestamp = 0;
	uint32_t priming = 0;
	std::vector<float> pending;
};

bool AacEldEncoder::available(std::string &reason)
{
	reason.clear();
	return true;
}

AacEldEncoder::AacEldEncoder() : impl_(new Impl) {}

AacEldEncoder::~AacEldEncoder()
{
	stop();
	delete impl_;
}

void AacEldEncoder::stop()
{
	if (impl_->converter) {
		AudioConverterDispose(impl_->converter);
		impl_->converter = nullptr;
	}
	impl_->pending.clear();
	impl_->timestamp = 0;
	priming_ = 0;
}

bool AacEldEncoder::start(uint32_t sample_rate, std::string &error)
{
	stop();
	if (sample_rate < 8000)
		sample_rate = 48000;
	AudioStreamBasicDescription input{};
	input.mSampleRate = sample_rate;
	input.mFormatID = kAudioFormatLinearPCM;
	input.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	input.mBitsPerChannel = 32;
	input.mChannelsPerFrame = 2;
	input.mFramesPerPacket = 1;
	input.mBytesPerFrame = 8;
	input.mBytesPerPacket = 8;

	AudioStreamBasicDescription output{};
	output.mSampleRate = sample_rate;
	output.mFormatID = kAudioFormatMPEG4AAC_ELD;
	output.mChannelsPerFrame = 2;
	output.mFramesPerPacket = 480;

	AudioConverterRef converter = nullptr;
	OSStatus status = AudioConverterNew(&input, &output, &converter);
	if (status != noErr || !converter) {
		error = "AAC-ELD encoder did not start (" + std::to_string((int)status) + ")";
		return false;
	}
	/* Screen-mirror frames on this TV are about 256 bytes, near 200 kbps. */
	UInt32 bitrate = 192000;
	UInt32 size = sizeof bitrate;
	AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, size, &bitrate);
	AudioStreamBasicDescription actual = output;
	size = sizeof actual;
	if (AudioConverterGetProperty(converter, kAudioConverterCurrentOutputStreamDescription, &size, &actual) == noErr &&
	    actual.mFramesPerPacket > 0)
		impl_->frame_size = (uint32_t)actual.mFramesPerPacket;
	AudioConverterPrimeInfo prime{};
	size = sizeof prime;
	if (AudioConverterGetProperty(converter, kAudioConverterPrimeInfo, &size, &prime) == noErr)
		impl_->priming = prime.leadingFrames;
	priming_ = impl_->priming;
	impl_->converter = converter;
	return true;
}

void AacEldEncoder::push(const float *interleaved, uint32_t frames, std::vector<AacPacket> &out)
{
	if (!impl_->converter || !interleaved || frames == 0)
		return;
	impl_->pending.insert(impl_->pending.end(), interleaved, interleaved + (size_t)frames * 2);
	const size_t stride = (size_t)impl_->frame_size * 2;
	size_t read = 0;
	while (impl_->pending.size() - read >= stride) {
		Feed feed;
		feed.data = impl_->pending.data() + read;
		feed.frames = impl_->frame_size;
		AudioBufferList buffers;
		std::memset(&buffers, 0, sizeof buffers);
		uint8_t storage[2048];
		buffers.mNumberBuffers = 1;
		buffers.mBuffers[0].mNumberChannels = 2;
		buffers.mBuffers[0].mData = storage;
		buffers.mBuffers[0].mDataByteSize = sizeof storage;
		UInt32 packets = 1;
		OSStatus status = AudioConverterFillComplexBuffer(impl_->converter, feed_input, &feed, &packets, &buffers,
								   nullptr);
		read += stride;
		if (status != noErr || packets == 0 || buffers.mBuffers[0].mDataByteSize == 0)
			continue;
		AacPacket packet;
		packet.data.assign(storage, storage + buffers.mBuffers[0].mDataByteSize);
		uint32_t stamp = impl_->timestamp;
		if (stamp >= impl_->priming)
			stamp -= impl_->priming;
		else
			stamp = 0;
		packet.rtp_timestamp = stamp;
		/* Each packet is one complete AAC-ELD frame, so the marker bit stays set. */
		packet.marker = true;
		impl_->timestamp += impl_->frame_size;
		out.push_back(std::move(packet));
	}
	if (read)
		impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + (std::ptrdiff_t)read);
}

} /* namespace airplay */
