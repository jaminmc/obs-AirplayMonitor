/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/mirror_packet.hpp"

#include <cstring>

namespace airplay {
namespace {

void store_u32_le(uint8_t *out, uint32_t value)
{
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)((value >> (8 * i)) & 0xff);
}

void store_u64_le(uint8_t *out, uint64_t value)
{
	for (int i = 0; i < 8; i++)
		out[i] = (uint8_t)((value >> (8 * i)) & 0xff);
}

void store_f32_le(uint8_t *out, float value)
{
	static_assert(sizeof(float) == 4, "float is 4 bytes");
	uint32_t bits = 0;
	std::memcpy(&bits, &value, 4);
	store_u32_le(out, bits);
}

} /* namespace */

uint64_t ntp_from_seconds(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	uint32_t whole = (uint32_t)seconds;
	double fraction = seconds - (double)whole;
	uint32_t frac = (uint32_t)(fraction * 4294967296.0);
	return ((uint64_t)whole << 32) | frac;
}

std::vector<uint8_t> build_mirror_packet(MirrorPayloadType type, uint64_t ntp, uint32_t width, uint32_t height,
					 const uint8_t *payload, size_t payload_len, bool keyframe, bool hevc)
{
	if (!payload)
		payload_len = 0;
	std::vector<uint8_t> packet(kMirrorHeaderSize + payload_len, 0);
	store_u32_le(packet.data(), (uint32_t)payload_len);
	packet[4] = (uint8_t)type;
	if (type == MirrorPayloadType::Video && keyframe)
		packet[5] = 0x10;
	if (type == MirrorPayloadType::Heartbeat) {
		packet[6] = 0x1e;
	} else if (type == MirrorPayloadType::Codec) {
		packet[6] = hevc ? 0x1e : 0x16;
		packet[7] = 0x01;
	}
	store_u64_le(packet.data() + 8, ntp);
	if (type == MirrorPayloadType::Codec) {
		store_f32_le(packet.data() + 16, (float)width);
		store_f32_le(packet.data() + 20, (float)height);
		store_f32_le(packet.data() + 40, (float)width);
		store_f32_le(packet.data() + 44, (float)height);
		store_f32_le(packet.data() + 56, (float)width);
		store_f32_le(packet.data() + 60, (float)height);
	}
	if (payload_len)
		std::memcpy(packet.data() + kMirrorHeaderSize, payload, payload_len);
	return packet;
}

} /* namespace airplay */
