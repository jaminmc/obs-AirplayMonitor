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

/* VideoToolbox encoder for the standalone probe, which runs without OBS. */

struct RawNv12 {
	const uint8_t *data = nullptr;
	size_t size = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t timestamp_ns = 0;
	bool force_keyframe = false;
};

class VtEncoder {
public:
	using Callback = std::function<void(EncodedAu &&)>;

	VtEncoder();
	~VtEncoder();

	bool start(const EncodeConfig &config, std::string &error);
	void stop();
	bool encode(const RawNv12 &frame, const Callback &callback, std::string &error);
	void finish();

private:
	struct Impl;
	Impl *impl_;
};

} /* namespace airplay */
