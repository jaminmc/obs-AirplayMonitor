/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AAC-ELD through fdk-aac, loaded at run time. Its license does not allow shipping it with a GPL
 * plugin, so audio works only where the user has installed the library.
 */

#include "audio/aac_eld.hpp"

#include "airplay/library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace airplay {
namespace {

/* The subset of aacenc_lib.h used here. */
using Handle = void *;
struct BufDesc {
	int num_bufs;
	void **bufs;
	int *identifiers;
	int *sizes;
	int *element_sizes;
};
struct InArgs {
	int num_in_samples;
	int num_anc_bytes;
};
struct OutArgs {
	int num_out_bytes;
	int num_in_samples;
	int num_anc_bytes;
	int bit_res_state;
};
/* fdk-aac 0.1 and 2.0 agree up to the delay field; the padding covers the larger layout. */
struct Info {
	unsigned max_out_buf_bytes;
	unsigned max_anc_bytes;
	unsigned in_buf_fill_level;
	unsigned input_channels;
	unsigned frame_length;
	unsigned delay;
	unsigned char rest[256];
};

constexpr unsigned kParamAot = 0x0100;
constexpr unsigned kParamBitrate = 0x0101;
constexpr unsigned kParamSampleRate = 0x0103;
constexpr unsigned kParamSbrMode = 0x0104;
constexpr unsigned kParamGranuleLength = 0x0105;
constexpr unsigned kParamChannelMode = 0x0106;
constexpr unsigned kParamAfterburner = 0x0200;
constexpr unsigned kParamTransmux = 0x0300;
constexpr unsigned kAotEld = 39;
constexpr unsigned kChannelModeStereo = 2;
constexpr unsigned kTransportRaw = 0;
constexpr int kInAudioData = 0;
constexpr int kOutBitstreamData = 3;

struct Library {
	int (*open)(Handle *, unsigned, unsigned) = nullptr;
	int (*close)(Handle *) = nullptr;
	int (*set_param)(Handle, unsigned, unsigned) = nullptr;
	int (*encode)(Handle, const BufDesc *, const BufDesc *, const InArgs *, OutArgs *) = nullptr;
	int (*info)(Handle, Info *) = nullptr;
	bool loaded = false;
	std::string error;
};

const Library &library()
{
	static Library lib;
	static std::once_flag once;
	std::call_once(once, []() {
#if defined(_WIN32)
		const char *names[] = {"libfdk-aac-2.dll", "fdk-aac.dll", "libfdk-aac.dll", "libfdk-aac-1.dll"};
#elif defined(__APPLE__)
		const char *names[] = {"libfdk-aac.2.dylib", "/opt/homebrew/lib/libfdk-aac.2.dylib",
				       "/usr/local/lib/libfdk-aac.2.dylib"};
#else
		const char *names[] = {"libfdk-aac.so.2", "libfdk-aac.so.1", "libfdk-aac.so"};
#endif
		void *module = nullptr;
		for (const char *name : names) {
			module = open_library(name);
			if (module)
				break;
		}
		if (!module) {
			lib.error = "fdk-aac is not installed";
			return;
		}
		if (!library_function(module, "aacEncOpen", lib.open) ||
		    !library_function(module, "aacEncClose", lib.close) ||
		    !library_function(module, "aacEncoder_SetParam", lib.set_param) ||
		    !library_function(module, "aacEncEncode", lib.encode) ||
		    !library_function(module, "aacEncInfo", lib.info)) {
			lib.error = "the installed fdk-aac has no AAC encoder";
			return;
		}
		lib.loaded = true;
	});
	return lib;
}

} /* namespace */

struct AacEldEncoder::Impl {
	Handle encoder = nullptr;
	uint32_t frame_size = 480;
	uint32_t timestamp = 0;
	uint32_t priming = 0;
	std::vector<int16_t> pending;
	std::vector<uint8_t> output;
};

bool AacEldEncoder::available(std::string &reason)
{
	const Library &lib = library();
	reason = lib.error;
	return lib.loaded;
}

AacEldEncoder::AacEldEncoder() : impl_(new Impl) {}

AacEldEncoder::~AacEldEncoder()
{
	stop();
	delete impl_;
}

void AacEldEncoder::stop()
{
	if (impl_->encoder)
		library().close(&impl_->encoder);
	impl_->encoder = nullptr;
	impl_->pending.clear();
	impl_->timestamp = 0;
	priming_ = 0;
}

bool AacEldEncoder::start(uint32_t sample_rate, std::string &error)
{
	stop();
	const Library &lib = library();
	if (!lib.loaded) {
		error = "AAC-ELD encoder did not start: " + lib.error;
		return false;
	}
	if (sample_rate < 8000)
		sample_rate = 48000;
	Handle encoder = nullptr;
	if (lib.open(&encoder, 0, 2) != 0 || !encoder) {
		error = "AAC-ELD encoder did not start (open)";
		return false;
	}
	const std::pair<unsigned, unsigned> params[] = {
		{kParamAot, kAotEld},
		{kParamSampleRate, sample_rate},
		{kParamChannelMode, kChannelModeStereo},
		{kParamGranuleLength, 480},
		{kParamTransmux, kTransportRaw},
		/* Screen-mirror frames on this TV are about 256 bytes, near 200 kbps. */
		{kParamBitrate, 192000},
		{kParamAfterburner, 1},
	};
	for (const auto &param : params) {
		if (lib.set_param(encoder, param.first, param.second) != 0) {
			lib.close(&encoder);
			char hex[8];
			std::snprintf(hex, sizeof hex, "%04x", param.first);
			error = std::string("AAC-ELD encoder rejected setting 0x") + hex;
			return false;
		}
	}
	/* Off is already the ELD default here, and builds without SBR reject the setting entirely. */
	lib.set_param(encoder, kParamSbrMode, 0);
	Info info{};
	if (lib.encode(encoder, nullptr, nullptr, nullptr, nullptr) != 0 || lib.info(encoder, &info) != 0) {
		lib.close(&encoder);
		error = "AAC-ELD encoder did not start (initialise)";
		return false;
	}
	impl_->frame_size = info.frame_length ? info.frame_length : 480;
	impl_->priming = info.delay;
	impl_->output.resize(std::max<unsigned>(info.max_out_buf_bytes, 2048));
	priming_ = impl_->priming;
	impl_->encoder = encoder;
	return true;
}

void AacEldEncoder::push(const float *interleaved, uint32_t frames, std::vector<AacPacket> &out)
{
	if (!impl_->encoder || !interleaved || frames == 0)
		return;
	const Library &lib = library();
	size_t samples = (size_t)frames * 2;
	size_t base = impl_->pending.size();
	impl_->pending.resize(base + samples);
	for (size_t i = 0; i < samples; i++) {
		float value = std::clamp(interleaved[i], -1.0f, 1.0f);
		impl_->pending[base + i] = (int16_t)std::lrintf(value * 32767.0f);
	}
	const size_t stride = (size_t)impl_->frame_size * 2;
	size_t read = 0;
	while (impl_->pending.size() - read >= stride) {
		void *in_ptr = impl_->pending.data() + read;
		int in_id = kInAudioData;
		int in_size = (int)(stride * sizeof(int16_t));
		int in_element = (int)sizeof(int16_t);
		BufDesc in_desc{1, &in_ptr, &in_id, &in_size, &in_element};
		void *out_ptr = impl_->output.data();
		int out_id = kOutBitstreamData;
		int out_size = (int)impl_->output.size();
		int out_element = 1;
		BufDesc out_desc{1, &out_ptr, &out_id, &out_size, &out_element};
		InArgs in_args{(int)stride, 0};
		OutArgs out_args{};
		int status = lib.encode(impl_->encoder, &in_desc, &out_desc, &in_args, &out_args);
		size_t consumed = out_args.num_in_samples > 0 ? (size_t)out_args.num_in_samples : stride;
		read += std::min(consumed, stride);
		if (status != 0 || out_args.num_out_bytes <= 0)
			continue;
		AacPacket packet;
		packet.data.assign(impl_->output.begin(), impl_->output.begin() + out_args.num_out_bytes);
		uint32_t stamp = impl_->timestamp;
		packet.rtp_timestamp = stamp >= impl_->priming ? stamp - impl_->priming : 0;
		/* Each packet is one complete AAC-ELD frame, so the marker bit stays set. */
		packet.marker = true;
		impl_->timestamp += impl_->frame_size;
		out.push_back(std::move(packet));
	}
	if (read)
		impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + (std::ptrdiff_t)read);
}

} /* namespace airplay */
