/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

struct AacPacket {
	std::vector<uint8_t> data;
	uint32_t rtp_timestamp = 0;
	bool marker = false;
};

class AacEldEncoder {
public:
	AacEldEncoder();
	~AacEldEncoder();

	/* False with a reason when no AAC-ELD encoder exists on this system. */
	static bool available(std::string &reason);

	bool start(uint32_t sample_rate, std::string &error);
	void stop();
	/* Interleaved float stereo at the rate passed to start. */
	void push(const float *interleaved, uint32_t frames, std::vector<AacPacket> &out);
	uint32_t priming_samples() const { return priming_; }

private:
	struct Impl;
	Impl *impl_;
	uint32_t priming_ = 0;
};

} /* namespace airplay */
