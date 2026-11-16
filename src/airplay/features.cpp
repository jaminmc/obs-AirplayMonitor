/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/features.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace airplay {
namespace {

uint32_t even_down(uint32_t value)
{
	uint32_t even = value & ~1u;
	return even < 2 ? 2 : even;
}

uint64_t parse_hex(const std::string &text, size_t begin, size_t end)
{
	if (begin >= end)
		return 0;
	std::string slice = text.substr(begin, end - begin);
	return std::strtoull(slice.c_str(), nullptr, 16);
}

} /* namespace */

bool feature_bit(uint64_t features, int bit)
{
	if (bit < 0 || bit > 63)
		return false;
	return (features & (1ull << bit)) != 0;
}

uint64_t parse_features_txt(const std::string &text)
{
	std::string hex;
	hex.reserve(text.size());
	for (char ch : text) {
		if (ch != ' ' && ch != '\t')
			hex.push_back(ch);
	}
	if (hex.empty())
		return 0;
	auto comma = hex.find(',');
	if (comma == std::string::npos)
		return parse_hex(hex, 0, hex.size());
	uint64_t low = parse_hex(hex, 0, comma);
	uint64_t high = parse_hex(hex, comma + 1, hex.size());
	return (high << 32) | (low & 0xffffffffull);
}

bool supports_screen(uint64_t features)
{
	return feature_bit(features, (int)FeatureBit::Screen);
}

bool requires_one_time_code(uint32_t flags)
{
	return (flags & kReceiverFlagOneTimeB) != 0;
}

int pin_hkp(uint64_t features)
{
	bool core = feature_bit(features, (int)FeatureBit::CoreUtilsPairing) ||
		    feature_bit(features, (int)FeatureBit::SystemPairing) ||
		    feature_bit(features, (int)FeatureBit::HomeKitPairing) ||
		    feature_bit(features, (int)FeatureBit::TransientPairing);
	bool third_party = feature_bit(features, (int)FeatureBit::ThirdPartyPairing) ||
			   feature_bit(features, (int)FeatureBit::UnifiedPairing);
	if (core && !third_party)
		return 5;
	return 3;
}

bool requires_pairing(uint64_t features, uint32_t flags)
{
	return requires_one_time_code(flags) || feature_bit(features, (int)FeatureBit::TransientPairing);
}

bool requires_fairplay(uint64_t features, bool paired)
{
	return paired || feature_bit(features, (int)FeatureBit::FairPlayAesGcm) ||
	       feature_bit(features, (int)FeatureBit::FairPlayAuth) ||
	       feature_bit(features, (int)FeatureBit::UnifiedPairing);
}

bool advertise_hevc(uint64_t features, const std::vector<DisplayMode> &displays)
{
	if (feature_bit(features, (int)FeatureBit::ScreenMultiCodec))
		return true;
	for (const DisplayMode &display : displays) {
		if (display.hevc || feature_bit(display.features, (int)FeatureBit::ScreenMultiCodec))
			return true;
	}
	return false;
}

RejectReason reject_reason(uint64_t features)
{
	if (supports_screen(features))
		return RejectReason::None;
	bool audio = feature_bit(features, (int)FeatureBit::Audio);
	bool video = feature_bit(features, (int)FeatureBit::Video) ||
		     feature_bit(features, (int)FeatureBit::VideoHttpLive);
	if (video)
		return RejectReason::VideoUrlOnly;
	if (audio)
		return RejectReason::AudioOnly;
	return RejectReason::NoScreen;
}

const char *reject_text(RejectReason reason)
{
	switch (reason) {
	case RejectReason::None:
		return "Screen mirroring";
	case RejectReason::AudioOnly:
		return "Audio only";
	case RejectReason::VideoUrlOnly:
		return "Video URL playback only";
	case RejectReason::NoScreen:
		return "No screen mirroring";
	}
	return "No screen mirroring";
}

const char *codec_name(Codec codec)
{
	return codec == Codec::Hevc ? "HEVC" : "H.264";
}

DisplayMode best_display(const std::vector<DisplayMode> &displays)
{
	DisplayMode best;
	uint64_t best_pixels = 0;
	for (const DisplayMode &display : displays) {
		uint64_t pixels = (uint64_t)display.width * display.height;
		if (pixels >= best_pixels) {
			best = display;
			best_pixels = pixels;
		}
	}
	return best;
}

EncodeConfig choose_encode(uint32_t src_w, uint32_t src_h, uint32_t src_fps, uint32_t max_w, uint32_t max_h,
			   uint32_t max_fps, bool hevc_ok, uint32_t bitrate_override_bps)
{
	if (src_w == 0)
		src_w = 1280;
	if (src_h == 0)
		src_h = 720;
	if (src_fps == 0)
		src_fps = 30;
	if (max_w == 0)
		max_w = src_w;
	if (max_h == 0)
		max_h = src_h;
	if (max_fps == 0)
		max_fps = src_fps;

	double scale = std::min(1.0, std::min((double)max_w / (double)src_w, (double)max_h / (double)src_h));
	if (!(scale > 0))
		scale = 1;

	EncodeConfig config;
	config.codec = hevc_ok ? Codec::Hevc : Codec::H264;
	config.width = even_down((uint32_t)std::floor((double)src_w * scale));
	config.height = even_down((uint32_t)std::floor((double)src_h * scale));
	config.fps = std::min(src_fps, max_fps);
	if (config.fps == 0)
		config.fps = 30;

	double reference = 1920.0 * 1080.0 * 60.0;
	double pixels = (double)config.width * (double)config.height * (double)config.fps;
	double rate = 12000000.0 * (pixels / reference);
	if (config.codec == Codec::Hevc)
		rate *= 0.6;
	rate = std::clamp(rate, 1500000.0, 40000000.0);
	if (bitrate_override_bps > 0)
		rate = bitrate_override_bps;
	config.bitrate = (uint32_t)std::llround(rate);
	return config;
}

} /* namespace airplay */
