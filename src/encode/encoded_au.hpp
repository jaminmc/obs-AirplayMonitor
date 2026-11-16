/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <vector>

namespace airplay {

struct EncodedAu {
	/* 4-byte length-prefixed NAL units. */
	std::vector<uint8_t> data;
	/* avcC or hvcC, set on the first keyframe and whenever the parameter sets change. */
	std::vector<uint8_t> codec_config;
	/* Big-endian ImageDescription. A Mac AirPlay receiver parses this instead of the raw avcC or hvcC. */
	std::vector<uint8_t> sample_description;
	bool keyframe = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t encode_ns = 0;
};

} /* namespace airplay */
