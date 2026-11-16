/* SPDX-License-Identifier: LGPL-3.0-or-later
 * C++ port, made 2026-11-15 for AirPlay Monitor, of the Go packages fpbridge, fpsapcore and
 * fairplayhash from github.com/objevovat/fairplay-sap-core-airplay2-sender-authentication-handshake.
 * fpsapcore and parts of fpbridge derive from github.com/omarroth/doubletake at 8ccea5f.
 *
 * The m1 record, a per-session m3 for a mode-3 m2, and the two keys that wrap the stream key.
 * See ../../NOTICE.md.
 *
 * Freestanding apart from ../c/fairplay_sapcore.c. The caller supplies all randomness, and the
 * stream key wrap itself (AES-128 and HMAC-SHA1) is left to the caller's own crypto.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace fairplay_sap {

constexpr size_t kM1Size = 16;
constexpr size_t kM2Size = 142;
constexpr size_t kM3Size = 164;
/* The local SAP is 00 01 followed by this many random bytes. */
constexpr size_t kLocalSapEntropy = 126;

/* The 16-byte m1 that opens the exchange, advertising full capabilities. */
void build_m1(uint8_t out[kM1Size]);

/* The 164-byte m3 answering a receiver's mode-3 m2. On failure returns false and sets *error. */
bool build_m3(const uint8_t *m2, size_t m2_size, const uint8_t entropy[kLocalSapEntropy], uint8_t out[kM3Size],
	      const char **error);

/* Keys for the 72-byte ekey: aes_key encrypts the masked stream key, mac_key is the HMAC-SHA1 key. */
bool wrap_keys(const uint8_t *m2, size_t m2_size, const uint8_t *m3, size_t m3_size, uint8_t aes_key[16],
	       uint8_t mac_key[20], const char **error);

} /* namespace fairplay_sap */
