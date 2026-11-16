/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <vector>

namespace airplay {

std::vector<uint8_t> build_rtp_audio(uint16_t sequence, uint32_t timestamp, uint32_t ssrc, const uint8_t *payload,
				     size_t size, bool marker);

/* The Roku plays this 20-byte sync. The NTP value is the same clock as the timing replies. */
std::vector<uint8_t> classic_sync_packet(uint32_t playing, uint32_t rtp, uint64_t ntp, bool first);

/* The RTP time due now, on a clock fixed when the first packet was captured.
   Stamping a sync from a packet that left late moves the TV's audio later,
   and the TV never moves it back. */
class AudioSyncClock {
public:
	/* rtp_time and capture_ns belong to the packet being sent. capture_ns is on
	   the same clock as now_ns. Zero, or a time over a second old, means now. */
	uint32_t due(uint32_t rtp_time, uint64_t capture_ns, uint64_t now_ns, uint32_t rate);

private:
	uint64_t anchor_ns_ = 0;
	uint32_t anchor_rtp_ = 0;
};

} /* namespace airplay */
