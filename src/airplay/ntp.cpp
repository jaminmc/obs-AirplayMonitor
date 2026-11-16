/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/ntp.hpp"

#include <cstring>

namespace airplay {

void store_ntp_be(uint8_t *out, uint64_t ntp)
{
	for (int i = 0; i < 8; i++)
		out[i] = (uint8_t)((ntp >> (56 - 8 * i)) & 0xff);
}

std::vector<uint8_t> ntp_reply(const uint8_t *request, size_t len, uint64_t now)
{
	if (!request || len < 48)
		return {};
	int mode = request[0] & 0x7;
	if (mode != 3)
		return {};
	std::vector<uint8_t> reply(48, 0);
	std::memcpy(reply.data(), request, 48);
	reply[0] = 0x24; /* LI=0, version 4, server mode */
	reply[1] = 1;    /* primary reference */
	reply[2] = request[2];
	reply[3] = (uint8_t)0xe8; /* ~1 microsecond precision */
	reply[12] = 'A';
	reply[13] = 'I';
	reply[14] = 'R';
	reply[15] = 'P';
	/* origin = client's transmit timestamp */
	std::memcpy(reply.data() + 24, request + 40, 8);
	store_ntp_be(reply.data() + 32, now);
	store_ntp_be(reply.data() + 40, now);
	return reply;
}

std::vector<uint8_t> airplay_timing_reply(const uint8_t *request, size_t len, uint64_t now)
{
	if (!request || len < 32 || request[0] != 0x80 || request[1] != 0xd2)
		return {};
	std::vector<uint8_t> reply(32, 0);
	std::memcpy(reply.data(), request, 32);
	reply[0] = 0x80;
	reply[1] = 0xd3;
	/* Reference timestamp is the sender's transmit time. */
	std::memcpy(reply.data() + 8, request + 24, 8);
	constexpr uint64_t kEpoch1900 = 2208988800ull;
	uint64_t stamped = now + (kEpoch1900 << 32);
	store_ntp_be(reply.data() + 16, stamped);
	store_ntp_be(reply.data() + 24, stamped);
	return reply;
}

} /* namespace airplay */
