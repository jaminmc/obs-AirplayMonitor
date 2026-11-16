/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

void sha512(const uint8_t *data, size_t size, uint8_t out[64]);
void sha512_parts(const uint8_t *const *parts, const size_t *sizes, size_t count, uint8_t out[64]);
void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t size, uint8_t out[64]);

/* HKDF-SHA512, one block. `out_len` is at most 64. */
bool hkdf_sha512(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info,
		 size_t info_len, uint8_t *out, size_t out_len);

/* IETF ChaCha20-Poly1305. `out` is ciphertext followed by the 16-byte tag. */
bool aead_seal(const uint8_t key[32], uint64_t counter, const uint8_t *aad, size_t aad_len, const uint8_t *plain,
	       size_t plain_len, std::vector<uint8_t> &out);
bool aead_open(const uint8_t key[32], uint64_t counter, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
	       size_t sealed_len, std::vector<uint8_t> &out);

/* Same AEAD with a caller-supplied 12-byte nonce. HAP pairing puts an 8-byte
 * label at offset 4 and leaves the first 4 bytes zero. That is not the counter
 * nonce used by aead_seal.
 */
bool aead_seal_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		     const uint8_t *plain, size_t plain_len, std::vector<uint8_t> &out);
bool aead_open_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		     const uint8_t *sealed, size_t sealed_len, std::vector<uint8_t> &out);

/* RFC 8439 ChaCha20, Poly1305, and AEAD vectors, plus a counter-nonce round trip. */
bool aead_self_test();

} /* namespace airplay */
