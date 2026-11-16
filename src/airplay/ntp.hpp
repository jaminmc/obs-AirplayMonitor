/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <vector>

namespace airplay {

/* Reply to a 48-byte NTP client packet. `now` is session-relative NTP
 * (seconds in the high 32 bits), written big-endian on the wire.
 * Returns an empty vector when the request is not a client packet.
 */
std::vector<uint8_t> ntp_reply(const uint8_t *request, size_t len, uint64_t now);

/* AirPlay timing is a 32-byte packet, marker 0x80 and type 0xd2. The reply is
 * type 0xd3. `now` is boot-relative NTP (seconds in the high 32 bits). The
 * reply adds the 1900 epoch; video headers stay boot-relative.
 */
std::vector<uint8_t> airplay_timing_reply(const uint8_t *request, size_t len, uint64_t now);

void store_ntp_be(uint8_t *out, uint64_t ntp);

} /* namespace airplay */
