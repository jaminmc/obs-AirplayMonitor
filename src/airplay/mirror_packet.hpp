/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <vector>

namespace airplay {

constexpr size_t kMirrorHeaderSize = 128;

enum class MirrorPayloadType : uint16_t { Video = 0, Codec = 1, Heartbeat = 2 };

/* NTP timestamp is the 64-bit value (seconds in the high 32 bits, fraction in
 * the low 32 bits), stored little-endian, counted from the start of the session.
 * Byte 4 is the payload kind. Byte 5 is 0x10 for an IDR video frame. Bytes 6-7
 * are 0x16 0x01 for H.264 parameter sets, 0x1e 0x01 for HEVC, 0x1e 0x00 for a
 * heartbeat, and 0x00 0x00 for encrypted video. Codec packets carry the picture
 * size as little-endian floats. The payload is copied as given.
 */
std::vector<uint8_t> build_mirror_packet(MirrorPayloadType type, uint64_t ntp, uint32_t width, uint32_t height,
					 const uint8_t *payload, size_t payload_len, bool keyframe = false,
					 bool hevc = false);

uint64_t ntp_from_seconds(double seconds);

} /* namespace airplay */
