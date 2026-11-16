/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

enum class FeatureBit : int {
	Video = 0,
	Photo = 1,
	VideoHttpLive = 4,
	Screen = 7,
	Audio = 9,
	FairPlayAesGcm = 12,
	FairPlayAuth = 14,
	ThirdPartyPairing = 26,
	LegacyPairing = 27,
	CoreUtilsPairing = 38,
	ScreenMultiCodec = 42,
	SystemPairing = 43,
	HomeKitPairing = 46,
	TransientPairing = 48,
	UnifiedPairing = 51,
};

/* TXT `flags`. Bit 9 (0x200) is the on-screen one-time code. Bit 6 (0x40) is not. */
constexpr uint32_t kReceiverFlagOneTimeA = 0x40;
constexpr uint32_t kReceiverFlagOneTimeB = 0x200;

enum class Codec { H264, Hevc };

enum class RejectReason { None, NoScreen, AudioOnly, VideoUrlOnly };

struct DisplayMode {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t max_fps = 0;
	uint64_t features = 0;
	bool hevc = false;
};

struct ReceiverInfo {
	std::string name;
	std::string model;
	std::string device_id;
	std::string host;
	std::string address;
	uint16_t port = 7000;
	int family = 2; /* AF_INET, filled by discovery */
	uint64_t features = 0;
	uint32_t status_flags = 0;
	std::string pk_hex;
	std::string srcvers;
	std::vector<DisplayMode> displays;
};

struct EncodeConfig {
	Codec codec = Codec::H264;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps = 30;
	uint32_t bitrate = 0;
	bool fell_back_from_hevc = false;
};

bool feature_bit(uint64_t features, int bit);
uint64_t parse_features_txt(const std::string &text);
bool supports_screen(uint64_t features);
bool advertise_hevc(uint64_t features, const std::vector<DisplayMode> &displays);
bool requires_one_time_code(uint32_t flags);
bool requires_pairing(uint64_t features, uint32_t flags);
bool requires_fairplay(uint64_t features, bool paired);
/* 5 for a first-party receiver, 3 when bit 26 or 51 marks it third-party. */
int pin_hkp(uint64_t features);
RejectReason reject_reason(uint64_t features);
const char *reject_text(RejectReason reason);
const char *codec_name(Codec codec);

EncodeConfig choose_encode(uint32_t src_w, uint32_t src_h, uint32_t src_fps, uint32_t max_w, uint32_t max_h,
			   uint32_t max_fps, bool hevc_ok, uint32_t bitrate_override_bps);

DisplayMode best_display(const std::vector<DisplayMode> &displays);

} /* namespace airplay */
