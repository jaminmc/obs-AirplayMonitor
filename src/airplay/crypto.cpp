/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/crypto.hpp"

#include "airplay/portable.hpp"

#include <algorithm>
#include <cstring>

namespace airplay {
namespace {

const uint8_t kSbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82,
	0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
	0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96,
	0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
	0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
	0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff,
	0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32,
	0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6,
	0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e,
	0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
	0xb0, 0x54, 0xbb, 0x16,
};

uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

uint32_t sub_word(uint32_t w)
{
	return (uint32_t)kSbox[w >> 24] << 24 | (uint32_t)kSbox[(w >> 16) & 0xff] << 16 |
	       (uint32_t)kSbox[(w >> 8) & 0xff] << 8 | kSbox[w & 0xff];
}

void aes128_expand(const uint8_t key[16], uint32_t rk[44])
{
	for (int i = 0; i < 4; i++)
		rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 |
			key[4 * i + 3];
	uint8_t rcon = 1;
	for (int i = 4; i < 44; i++) {
		uint32_t t = rk[i - 1];
		if (i % 4 == 0) {
			t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
			rcon = xtime(rcon);
		}
		rk[i] = rk[i - 4] ^ t;
	}
}

void aes128_encrypt(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	auto add_round_key = [&](int round) {
		for (int c = 0; c < 4; c++) {
			uint32_t k = rk[round * 4 + c];
			s[4 * c] ^= (uint8_t)(k >> 24);
			s[4 * c + 1] ^= (uint8_t)(k >> 16);
			s[4 * c + 2] ^= (uint8_t)(k >> 8);
			s[4 * c + 3] ^= (uint8_t)k;
		}
	};
	std::memcpy(s, in, 16);
	add_round_key(0);
	for (int round = 1; round <= 10; round++) {
		uint8_t t[16];
		for (int i = 0; i < 16; i++)
			s[i] = kSbox[s[i]];
		/* ShiftRows: byte r of column c comes from column c + r. */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[4 * c + r] = s[4 * ((c + r) % 4) + r];
		if (round < 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = t + 4 * c;
				uint8_t all = col[0] ^ col[1] ^ col[2] ^ col[3];
				uint8_t first = col[0];
				col[0] ^= all ^ xtime(col[0] ^ col[1]);
				col[1] ^= all ^ xtime(col[1] ^ col[2]);
				col[2] ^= all ^ xtime(col[2] ^ col[3]);
				col[3] ^= all ^ xtime(col[3] ^ first);
			}
		}
		std::memcpy(s, t, 16);
		add_round_key(round);
	}
	std::memcpy(out, s, 16);
}

uint32_t rotl32(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

/* Little-endian 32-bit limbs, enough for the one public-key operation RSA-OAEP needs. */
using Big = std::vector<uint32_t>;

Big big_from_be(const uint8_t *data, size_t size)
{
	Big out((size + 3) / 4, 0);
	for (size_t i = 0; i < size; i++) {
		size_t from_end = size - 1 - i;
		out[from_end / 4] |= (uint32_t)data[i] << (8 * (from_end % 4));
	}
	return out;
}

size_t big_bits(const Big &a)
{
	for (size_t i = a.size(); i-- > 0;) {
		if (a[i]) {
			size_t bits = i * 32;
			uint32_t v = a[i];
			while (v) {
				bits++;
				v >>= 1;
			}
			return bits;
		}
	}
	return 0;
}

int big_cmp(const Big &a, const Big &b)
{
	size_t n = std::max(a.size(), b.size());
	for (size_t i = n; i-- > 0;) {
		uint32_t x = i < a.size() ? a[i] : 0;
		uint32_t y = i < b.size() ? b[i] : 0;
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

void big_sub(Big &a, const Big &b)
{
	int64_t borrow = 0;
	for (size_t i = 0; i < a.size(); i++) {
		int64_t diff = (int64_t)a[i] - (i < b.size() ? b[i] : 0) - borrow;
		borrow = diff < 0;
		a[i] = (uint32_t)(diff + (borrow ? (1ll << 32) : 0));
	}
}

Big big_mulmod(const Big &a, const Big &b, const Big &n)
{
	std::vector<uint64_t> wide(a.size() + b.size() + 1, 0);
	for (size_t i = 0; i < a.size(); i++) {
		uint64_t carry = 0;
		for (size_t j = 0; j < b.size(); j++) {
			uint64_t cur = wide[i + j] + (uint64_t)a[i] * b[j] + carry;
			wide[i + j] = cur & 0xffffffffu;
			carry = cur >> 32;
		}
		wide[i + b.size()] += carry;
	}
	Big product(wide.size());
	for (size_t i = 0; i < wide.size(); i++)
		product[i] = (uint32_t)wide[i];
	Big rem(n.size() + 1, 0);
	for (size_t bit = big_bits(product); bit-- > 0;) {
		uint32_t carry = (product[bit / 32] >> (bit % 32)) & 1;
		for (size_t i = 0; i < rem.size(); i++) {
			uint32_t next = rem[i] >> 31;
			rem[i] = (rem[i] << 1) | carry;
			carry = next;
		}
		if (big_cmp(rem, n) >= 0)
			big_sub(rem, n);
	}
	rem.resize(n.size());
	return rem;
}

Big big_powmod(const Big &base, const Big &exp, const Big &n)
{
	Big result(n.size(), 0);
	result[0] = 1;
	for (size_t bit = big_bits(exp); bit-- > 0;) {
		result = big_mulmod(result, result, n);
		if ((exp[bit / 32] >> (bit % 32)) & 1)
			result = big_mulmod(result, base, n);
	}
	return result;
}

bool der_read(const std::vector<uint8_t> &der, size_t &pos, uint8_t tag, size_t &start, size_t &length)
{
	if (pos + 2 > der.size() || der[pos] != tag)
		return false;
	size_t len = der[pos + 1];
	size_t at = pos + 2;
	if (len & 0x80) {
		size_t count = len & 0x7f;
		if (count == 0 || count > 4 || at + count > der.size())
			return false;
		len = 0;
		for (size_t i = 0; i < count; i++)
			len = (len << 8) | der[at++];
	}
	if (at + len > der.size())
		return false;
	start = at;
	length = len;
	pos = at + len;
	return true;
}

/* Reads a PKCS#1 RSAPublicKey, or the same key inside a SubjectPublicKeyInfo. */
bool rsa_public_from_der(const std::vector<uint8_t> &der, std::vector<uint8_t> &modulus, std::vector<uint8_t> &exponent)
{
	size_t pos = 0, start = 0, length = 0;
	if (!der_read(der, pos, 0x30, start, length))
		return false;
	size_t inner = start;
	if (inner < der.size() && der[inner] == 0x30) {
		size_t algorithm_start = 0, algorithm_length = 0;
		if (!der_read(der, inner, 0x30, algorithm_start, algorithm_length) ||
		    !der_read(der, inner, 0x03, start, length) || length < 1)
			return false;
		std::vector<uint8_t> key(der.begin() + (long)start + 1, der.begin() + (long)(start + length));
		return rsa_public_from_der(key, modulus, exponent);
	}
	size_t n_start = 0, n_length = 0, e_start = 0, e_length = 0;
	if (!der_read(der, inner, 0x02, n_start, n_length) || !der_read(der, inner, 0x02, e_start, e_length))
		return false;
	while (n_length > 1 && der[n_start] == 0) {
		n_start++;
		n_length--;
	}
	modulus.assign(der.begin() + (long)n_start, der.begin() + (long)(n_start + n_length));
	exponent.assign(der.begin() + (long)e_start, der.begin() + (long)(e_start + e_length));
	return true;
}

void mgf1_sha1(const uint8_t *seed, size_t seed_len, uint8_t *mask, size_t mask_len)
{
	std::vector<uint8_t> input(seed, seed + seed_len);
	input.resize(seed_len + 4);
	for (uint32_t counter = 0, done = 0; done < mask_len; counter++) {
		input[seed_len] = (uint8_t)(counter >> 24);
		input[seed_len + 1] = (uint8_t)(counter >> 16);
		input[seed_len + 2] = (uint8_t)(counter >> 8);
		input[seed_len + 3] = (uint8_t)counter;
		uint8_t digest[20];
		sha1(input.data(), input.size(), digest);
		for (size_t i = 0; i < 20 && done < mask_len; i++)
			mask[done++] ^= digest[i];
	}
}

} /* namespace */

void sha1(const uint8_t *data, size_t size, uint8_t out[20])
{
	uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	std::vector<uint8_t> message(data, data + size);
	message.push_back(0x80);
	while (message.size() % 64 != 56)
		message.push_back(0);
	uint64_t bits = (uint64_t)size * 8;
	for (int i = 7; i >= 0; i--)
		message.push_back((uint8_t)(bits >> (8 * i)));
	for (size_t offset = 0; offset < message.size(); offset += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++) {
			const uint8_t *p = message.data() + offset + 4 * i;
			w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
		}
		for (int i = 16; i < 80; i++)
			w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6;
			}
			uint32_t t = rotl32(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rotl32(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (int i = 0; i < 5; i++)
		for (int b = 0; b < 4; b++)
			out[i * 4 + b] = (uint8_t)(h[i] >> (24 - 8 * b));
}

void hmac_sha1(const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, uint8_t out[20])
{
	uint8_t block[64] = {};
	if (key_size > sizeof block)
		sha1(key, key_size, block);
	else
		std::memcpy(block, key, key_size);
	std::vector<uint8_t> inner(sizeof block + size);
	for (size_t i = 0; i < sizeof block; i++)
		inner[i] = block[i] ^ 0x36;
	std::memcpy(inner.data() + sizeof block, data, size);
	uint8_t outer[sizeof block + 20];
	for (size_t i = 0; i < sizeof block; i++)
		outer[i] = block[i] ^ 0x5c;
	sha1(inner.data(), inner.size(), outer + sizeof block);
	sha1(outer, sizeof outer, out);
	std::memset(block, 0, sizeof block);
}

void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint32_t rk[44];
	aes128_expand(key, rk);
	aes128_encrypt(rk, in, out);
	std::memset(rk, 0, sizeof rk);
}

AesCtr::AesCtr() = default;

AesCtr::~AesCtr()
{
	reset();
}

void AesCtr::reset()
{
	std::memset(round_keys_, 0, sizeof round_keys_);
	std::memset(counter_, 0, sizeof counter_);
	std::memset(stream_, 0, sizeof stream_);
	used_ = sizeof stream_;
	ready_ = false;
}

bool AesCtr::set_key(const uint8_t key[16], const uint8_t iv[16], std::string &error)
{
	reset();
	if (!key || !iv) {
		error = "AES-CTR setup failed";
		return false;
	}
	aes128_expand(key, round_keys_);
	std::memcpy(counter_, iv, sizeof counter_);
	ready_ = true;
	return true;
}

bool AesCtr::process(uint8_t *data, size_t size, std::string &error)
{
	if (!ready_ || !data) {
		error = "AES-CTR is not ready";
		return false;
	}
	for (size_t i = 0; i < size; i++) {
		if (used_ == sizeof stream_) {
			aes128_encrypt(round_keys_, counter_, stream_);
			for (int b = 15; b >= 0 && ++counter_[b] == 0; b--) {
			}
			used_ = 0;
		}
		data[i] ^= stream_[used_++];
	}
	return true;
}

bool hex_decode(const std::string &hex, std::vector<uint8_t> &out)
{
	std::string compact;
	compact.reserve(hex.size());
	for (char ch : hex) {
		if (ch == ' ' || ch == ':' || ch == '\n')
			continue;
		compact.push_back(ch);
	}
	if (compact.size() % 2)
		return false;
	out.clear();
	out.reserve(compact.size() / 2);
	auto nibble = [](char ch) -> int {
		if (ch >= '0' && ch <= '9')
			return ch - '0';
		if (ch >= 'a' && ch <= 'f')
			return ch - 'a' + 10;
		if (ch >= 'A' && ch <= 'F')
			return ch - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < compact.size(); i += 2) {
		int hi = nibble(compact[i]);
		int lo = nibble(compact[i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out.push_back((uint8_t)((hi << 4) | lo));
	}
	return true;
}

bool rsa_oaep_wrap_key(const std::string &pk_hex, const uint8_t key[16], std::vector<uint8_t> &out, std::string &error)
{
	std::vector<uint8_t> raw;
	if (!hex_decode(pk_hex, raw) || raw.size() < 128) {
		error = "Receiver key is not an RSA modulus";
		return false;
	}
	std::vector<uint8_t> modulus = raw;
	std::vector<uint8_t> exponent = {0x01, 0x00, 0x01};
	if (raw[0] == 0x30 && !rsa_public_from_der(raw, modulus, exponent)) {
		error = "Could not import the receiver RSA key";
		return false;
	}
	const size_t k = modulus.size();
	const size_t hash_len = 20;
	if (k < 128 || !(modulus.back() & 1) || 16 > k - 2 * hash_len - 2) {
		error = "Could not import the receiver RSA key";
		return false;
	}
	/* RFC 8017 EME-OAEP with SHA-1, MGF1-SHA-1 and an empty label. */
	std::vector<uint8_t> em(k, 0);
	uint8_t *seed = em.data() + 1;
	uint8_t *db = em.data() + 1 + hash_len;
	const size_t db_len = k - hash_len - 1;
	sha1(nullptr, 0, db);
	db[db_len - 16 - 1] = 0x01;
	std::memcpy(db + db_len - 16, key, 16);
	random_bytes(seed, hash_len);
	mgf1_sha1(seed, hash_len, db, db_len);
	mgf1_sha1(db, db_len, seed, hash_len);

	Big n = big_from_be(modulus.data(), modulus.size());
	Big e = big_from_be(exponent.data(), exponent.size());
	Big m = big_from_be(em.data(), em.size());
	m.resize(n.size(), 0);
	Big c = big_powmod(m, e, n);
	out.assign(k, 0);
	for (size_t i = 0; i < k; i++) {
		size_t from_end = k - 1 - i;
		out[i] = (uint8_t)(c[from_end / 4] >> (8 * (from_end % 4)));
	}
	return true;
}

} /* namespace airplay */
