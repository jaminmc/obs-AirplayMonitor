/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/features.hpp"
#include "encode/encoded_au.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace airplay {

struct VideoEncoderChoice {
	std::string id;
	std::string name;
	Codec codec = Codec::H264;
	bool hardware = false;
};

/* The H.264 and HEVC encoders OBS offers on this machine. */
std::vector<VideoEncoderChoice> video_encoder_choices();
/* Encoder ids for `codec` to try in order: `preferred` first, then hardware before software. */
std::vector<std::string> encoder_candidates(const std::string &preferred, Codec codec);
/* Whether HEVC may be negotiated: the chosen encoder is HEVC, or automatic and an HEVC encoder exists. */
bool encoder_allows_hevc(const std::string &preferred);

class ObsVideoEncoder {
public:
	using Callback = std::function<void(EncodedAu &&)>;

	ObsVideoEncoder();
	~ObsVideoEncoder();

	/* Encodes Program, or Preview through a separate view. Packets arrive on an OBS thread. */
	bool start(const std::string &encoder_id, const EncodeConfig &config, bool preview, Callback callback,
		   std::string &error);
	void update_preview_scene();
	void stop();
	bool running() const;
	std::string encoder_name() const;
	uint64_t frames() const;
	uint64_t dropped() const;

	static void register_output();

private:
	struct Impl;
	Impl *impl_;
};

} /* namespace airplay */
