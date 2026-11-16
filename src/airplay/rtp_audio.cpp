/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/rtp_audio.hpp"

#include <cstring>

namespace airplay {

std::vector<uint8_t> build_rtp_audio(uint16_t sequence, uint32_t timestamp, uint32_t ssrc, const uint8_t *payload,
				     size_t size, bool marker)
{
	std::vector<uint8_t> packet(12 + size, 0);
	packet[0] = 0x80;
	packet[1] = (uint8_t)((marker ? 0x80 : 0) | 96);
	packet[2] = (uint8_t)(sequence >> 8);
	packet[3] = (uint8_t)(sequence & 0xff);
	packet[4] = (uint8_t)(timestamp >> 24);
	packet[5] = (uint8_t)(timestamp >> 16);
	packet[6] = (uint8_t)(timestamp >> 8);
	packet[7] = (uint8_t)(timestamp);
	packet[8] = (uint8_t)(ssrc >> 24);
	packet[9] = (uint8_t)(ssrc >> 16);
	packet[10] = (uint8_t)(ssrc >> 8);
	packet[11] = (uint8_t)(ssrc);
	if (payload && size)
		std::memcpy(packet.data() + 12, payload, size);
	return packet;
}

std::vector<uint8_t> classic_sync_packet(uint32_t playing, uint32_t rtp, uint64_t ntp, bool first)
{
	std::vector<uint8_t> packet(20, 0);
	packet[0] = first ? 0x90 : 0x80;
	packet[1] = 0xd4;
	packet[3] = 4;
	for (int i = 0; i < 4; i++) {
		packet[4 + i] = (uint8_t)(playing >> (24 - 8 * i));
		packet[16 + i] = (uint8_t)(rtp >> (24 - 8 * i));
	}
	for (int i = 0; i < 8; i++)
		packet[8 + i] = (uint8_t)(ntp >> (56 - 8 * i));
	return packet;
}

uint32_t AudioSyncClock::due(uint32_t rtp_time, uint64_t capture_ns, uint64_t now_ns, uint32_t rate)
{
	uint64_t captured = capture_ns && capture_ns <= now_ns && now_ns - capture_ns < 1000000000ull ? capture_ns
												       : now_ns;
	if (!anchor_ns_) {
		anchor_ns_ = captured;
		anchor_rtp_ = rtp_time;
	}
	uint32_t due = anchor_rtp_ + (uint32_t)((now_ns - anchor_ns_) / 1000 * rate / 1000000);
	uint32_t packet_due = rtp_time + (uint32_t)((now_ns - captured) / 1000 * rate / 1000000);
	int32_t skew = (int32_t)(packet_due - due);
	if (skew > (int32_t)rate || skew < -(int32_t)rate) {
		/* The source restarted or stalled for over a second. Start a new clock. */
		anchor_ns_ = captured;
		anchor_rtp_ = rtp_time;
		due = packet_due;
	}
	return due;
}

} /* namespace airplay */
