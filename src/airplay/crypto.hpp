/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

class AesCtr {
public:
	AesCtr();
	~AesCtr();
	AesCtr(const AesCtr &) = delete;
	AesCtr &operator=(const AesCtr &) = delete;

	bool set_key(const uint8_t key[16], const uint8_t iv[16], std::string &error);
	bool process(uint8_t *data, size_t size, std::string &error);
	void reset();

private:
	uint32_t round_keys_[44] = {};
	/* Big-endian 128-bit counter, as CommonCrypto's kCCModeOptionCTR_BE. */
	uint8_t counter_[16] = {};
	uint8_t stream_[16] = {};
	size_t used_ = 16;
	bool ready_ = false;
};

void sha1(const uint8_t *data, size_t size, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, uint8_t out[20]);
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* RSA-OAEP-SHA1 encrypt of a 16-byte AES key.
 * `pk_hex` is either an SPKI/PKCS#1 DER blob in hex, or a raw modulus
 * (exponent 65537) of at least 128 bytes.
 */
bool rsa_oaep_wrap_key(const std::string &pk_hex, const uint8_t key[16], std::vector<uint8_t> &out, std::string &error);

bool hex_decode(const std::string &hex, std::vector<uint8_t> &out);

} /* namespace airplay */
