/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/features.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace airplay {

struct ParameterSets {
	std::vector<uint8_t> vps;
	std::vector<uint8_t> sps;
	std::vector<uint8_t> pps;
};

/* Rewrites one Annex B access unit as 4-byte length-prefixed NAL units. Parameter sets move into `sets`
   and access unit delimiters are dropped. Returns whether any NAL unit was found. */
bool annexb_to_length_prefixed(Codec codec, const uint8_t *data, size_t size, std::vector<uint8_t> &out,
			       ParameterSets &sets);

bool build_avcc(const ParameterSets &sets, std::vector<uint8_t> &out);
bool build_hvcc(const ParameterSets &sets, std::vector<uint8_t> &out);

/* The big-endian QuickTime ImageDescription VideoToolbox makes for an avcC or hvcC. */
std::vector<uint8_t> image_description(Codec codec, uint32_t width, uint32_t height,
				       const std::vector<uint8_t> &config);

} /* namespace airplay */
