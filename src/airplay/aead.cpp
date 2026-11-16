/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/aead.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace airplay {
namespace {

uint32_t load32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void store32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint32_t rotl(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

void quarter(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d)
{
	a += b;
	d ^= a;
	d = rotl(d, 16);
	c += d;
	b ^= c;
	b = rotl(b, 12);
	a += b;
	d ^= a;
	d = rotl(d, 8);
	c += d;
	b ^= c;
	b = rotl(b, 7);
}

void chacha_block(const uint32_t input[16], uint8_t out[64])
{
	uint32_t x[16];
	std::memcpy(x, input, sizeof x);
	for (int i = 0; i < 10; i++) {
		quarter(x[0], x[4], x[8], x[12]);
		quarter(x[1], x[5], x[9], x[13]);
		quarter(x[2], x[6], x[10], x[14]);
		quarter(x[3], x[7], x[11], x[15]);
		quarter(x[0], x[5], x[10], x[15]);
		quarter(x[1], x[6], x[11], x[12]);
		quarter(x[2], x[7], x[8], x[13]);
		quarter(x[3], x[4], x[9], x[14]);
	}
	for (int i = 0; i < 16; i++)
		store32(out + 4 * i, x[i] + input[i]);
}

/* IETF ChaCha20. `nonce` is 12 bytes. Block 0 is the Poly1305 key block. */
void chacha_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t block, const uint8_t *in, uint8_t *out,
		size_t len)
{
	uint32_t state[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
	for (int i = 0; i < 8; i++)
		state[4 + i] = load32(key + 4 * i);
	state[12] = block;
	state[13] = load32(nonce);
	state[14] = load32(nonce + 4);
	state[15] = load32(nonce + 8);
	uint8_t stream[64];
	size_t offset = 0;
	while (offset < len) {
		chacha_block(state, stream);
		state[12]++;
		size_t n = len - offset;
		if (n > 64)
			n = 64;
		if (!in) {
			std::memcpy(out + offset, stream, n);
		} else {
			size_t i = 0;
			for (; i + 8 <= n; i += 8) {
				uint64_t value = 0;
				uint64_t mask = 0;
				std::memcpy(&value, in + offset + i, 8);
				std::memcpy(&mask, stream + i, 8);
				value ^= mask;
				std::memcpy(out + offset + i, &value, 8);
			}
			for (; i < n; i++)
				out[offset + i] = (uint8_t)(in[offset + i] ^ stream[i]);
		}
		offset += n;
	}
}

void nonce_from_counter(uint64_t counter, uint8_t nonce[12])
{
	std::memset(nonce, 0, 12);
	for (int i = 0; i < 8; i++)
		nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}

/* 26-bit Poly1305. A full 16-byte block adds 2^128. A short final block carries
 * the 0x01 pad byte and does not add a second high bit. */
void poly_block(uint32_t &h0, uint32_t &h1, uint32_t &h2, uint32_t &h3, uint32_t &h4, uint32_t r0, uint32_t r1,
		uint32_t r2, uint32_t r3, uint32_t r4, const uint8_t block[16], uint32_t hibit)
{
	uint32_t s1 = r1 * 5;
	uint32_t s2 = r2 * 5;
	uint32_t s3 = r3 * 5;
	uint32_t s4 = r4 * 5;
	uint32_t t0 = load32(block);
	uint32_t t1 = load32(block + 4);
	uint32_t t2 = load32(block + 8);
	uint32_t t3 = load32(block + 12);
	h0 += t0 & 0x3ffffff;
	h1 += (uint32_t)(((((uint64_t)t1 << 32) | t0) >> 26) & 0x3ffffff);
	h2 += (uint32_t)(((((uint64_t)t2 << 32) | t1) >> 20) & 0x3ffffff);
	h3 += (uint32_t)(((((uint64_t)t3 << 32) | t2) >> 14) & 0x3ffffff);
	h4 += (t3 >> 8) | hibit;

	uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
	uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
	uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
	uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
	uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
	h0 = (uint32_t)d0 & 0x3ffffff;
	d1 += d0 >> 26;
	h1 = (uint32_t)d1 & 0x3ffffff;
	d2 += d1 >> 26;
	h2 = (uint32_t)d2 & 0x3ffffff;
	d3 += d2 >> 26;
	h3 = (uint32_t)d3 & 0x3ffffff;
	d4 += d3 >> 26;
	h4 = (uint32_t)d4 & 0x3ffffff;
	h0 += (uint32_t)(d4 >> 26) * 5;
	uint32_t c = h0 >> 26;
	h0 &= 0x3ffffff;
	h1 += c;
}

void poly1305_mac(const uint8_t otk[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{
	/* 26-bit limbs of the clamped r from RFC 8439. */
	uint32_t r0 = load32(otk) & 0x3ffffff;
	uint32_t r1 = (load32(otk + 3) >> 2) & 0x3ffff03;
	uint32_t r2 = (load32(otk + 6) >> 4) & 0x3ffc0ff;
	uint32_t r3 = (load32(otk + 9) >> 6) & 0x3f03fff;
	uint32_t r4 = (load32(otk + 12) >> 8) & 0x00fffff;
	uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
	size_t offset = 0;
	while (len - offset >= 16) {
		poly_block(h0, h1, h2, h3, h4, r0, r1, r2, r3, r4, msg + offset, 1u << 24);
		offset += 16;
	}
	if (offset < len) {
		uint8_t last[16] = {};
		size_t n = len - offset;
		std::memcpy(last, msg + offset, n);
		last[n] = 1;
		poly_block(h0, h1, h2, h3, h4, r0, r1, r2, r3, r4, last, 0);
	}
	h2 += h1 >> 26;
	h1 &= 0x3ffffff;
	h3 += h2 >> 26;
	h2 &= 0x3ffffff;
	h4 += h3 >> 26;
	h3 &= 0x3ffffff;
	h0 += (h4 >> 26) * 5;
	h4 &= 0x3ffffff;
	h1 += h0 >> 26;
	h0 &= 0x3ffffff;

	uint32_t g0 = h0 + 5;
	uint32_t g1 = h1 + (g0 >> 26);
	g0 &= 0x3ffffff;
	uint32_t g2 = h2 + (g1 >> 26);
	g1 &= 0x3ffffff;
	uint32_t g3 = h3 + (g2 >> 26);
	g2 &= 0x3ffffff;
	uint32_t g4 = h4 + (g3 >> 26) - (1u << 26);
	g3 &= 0x3ffffff;
	uint32_t mask = (g4 >> 31) - 1u;
	g0 &= mask;
	g1 &= mask;
	g2 &= mask;
	g3 &= mask;
	g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	uint32_t p0 = (h0 | (h1 << 26)) & 0xffffffffu;
	uint32_t p1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
	uint32_t p2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
	uint32_t p3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;
	uint64_t f0 = (uint64_t)p0 + load32(otk + 16);
	uint64_t f1 = (uint64_t)p1 + load32(otk + 20) + (f0 >> 32);
	uint64_t f2 = (uint64_t)p2 + load32(otk + 24) + (f1 >> 32);
	uint64_t f3 = (uint64_t)p3 + load32(otk + 28) + (f2 >> 32);
	store32(tag, (uint32_t)f0);
	store32(tag + 4, (uint32_t)f1);
	store32(tag + 8, (uint32_t)f2);
	store32(tag + 12, (uint32_t)f3);
}

void append_padded(std::vector<uint8_t> &out, const uint8_t *data, size_t len)
{
	if (!len)
		return;
	out.insert(out.end(), data, data + len);
	while (out.size() % 16)
		out.push_back(0);
}

void aead_tag(const uint8_t otk[32], const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
	      uint8_t tag[16])
{
	std::vector<uint8_t> mac;
	mac.reserve(((aad_len + 15) & ~15u) + ((ct_len + 15) & ~15u) + 16);
	append_padded(mac, aad, aad_len);
	append_padded(mac, ct, ct_len);
	uint8_t lens[16] = {};
	for (int i = 0; i < 8; i++) {
		lens[i] = (uint8_t)(aad_len >> (8 * i));
		lens[8 + i] = (uint8_t)(ct_len >> (8 * i));
	}
	mac.insert(mac.end(), lens, lens + 16);
	poly1305_mac(otk, mac.data(), mac.size(), tag);
}

bool seal_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		const uint8_t *plain, size_t plain_len, std::vector<uint8_t> &out)
{
	uint8_t otk[64];
	chacha_xor(key, nonce, 0, nullptr, otk, 64);
	out.assign(plain_len + 16, 0);
	if (plain_len)
		chacha_xor(key, nonce, 1, plain, out.data(), plain_len);
	aead_tag(otk, aad, aad_len, out.data(), plain_len, out.data() + plain_len);
	return true;
}

bool open_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		const uint8_t *sealed, size_t sealed_len, std::vector<uint8_t> &out)
{
	if (sealed_len < 16)
		return false;
	size_t plain_len = sealed_len - 16;
	uint8_t otk[64];
	chacha_xor(key, nonce, 0, nullptr, otk, 64);
	uint8_t tag[16];
	aead_tag(otk, aad, aad_len, sealed, plain_len, tag);
	uint8_t diff = 0;
	for (int i = 0; i < 16; i++)
		diff |= (uint8_t)(tag[i] ^ sealed[plain_len + i]);
	if (diff)
		return false;
	out.assign(plain_len, 0);
	if (plain_len)
		chacha_xor(key, nonce, 1, sealed, out.data(), plain_len);
	return true;
}

int hex_nibble(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

bool unhex(const char *text, std::vector<uint8_t> &out)
{
	out.clear();
	for (size_t i = 0; text[i];) {
		if (text[i] == ' ' || text[i] == '\n' || text[i] == ':') {
			i++;
			continue;
		}
		int hi = hex_nibble(text[i]);
		int lo = text[i + 1] ? hex_nibble(text[i + 1]) : -1;
		if (hi < 0 || lo < 0)
			return false;
		out.push_back((uint8_t)((hi << 4) | lo));
		i += 2;
	}
	return true;
}

const uint64_t kSha512K[80] = {
	0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
	0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
	0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
	0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
	0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
	0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
	0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
	0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
	0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
	0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
	0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
	0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
	0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
	0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
	0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
	0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
	0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
	0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
	0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
	0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

uint64_t rotr64(uint64_t v, int n)
{
	return (v >> n) | (v << (64 - n));
}

class Sha512 {
public:
	void update(const uint8_t *data, size_t size)
	{
		if (!data)
			return;
		total_ += size;
		while (size) {
			size_t take = std::min(size, sizeof buffer_ - used_);
			std::memcpy(buffer_ + used_, data, take);
			used_ += take;
			data += take;
			size -= take;
			if (used_ == sizeof buffer_) {
				block(buffer_);
				used_ = 0;
			}
		}
	}

	void finish(uint8_t out[64])
	{
		uint64_t bits = total_ * 8;
		uint8_t pad = 0x80;
		uint64_t saved = total_;
		update(&pad, 1);
		const uint8_t zero = 0;
		while (used_ != 112)
			update(&zero, 1);
		uint8_t length[16] = {};
		for (int i = 0; i < 8; i++)
			length[15 - i] = (uint8_t)(bits >> (8 * i));
		update(length, sizeof length);
		total_ = saved;
		for (int i = 0; i < 8; i++)
			for (int b = 0; b < 8; b++)
				out[i * 8 + b] = (uint8_t)(h_[i] >> (56 - 8 * b));
	}

private:
	void block(const uint8_t *p)
	{
		uint64_t w[80];
		for (int i = 0; i < 16; i++) {
			w[i] = 0;
			for (int b = 0; b < 8; b++)
				w[i] = (w[i] << 8) | p[i * 8 + b];
		}
		for (int i = 16; i < 80; i++) {
			uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
			uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
		for (int i = 0; i < 80; i++) {
			uint64_t t1 = h + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) +
				      kSha512K[i] + w[i];
			uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h_[0] += a;
		h_[1] += b;
		h_[2] += c;
		h_[3] += d;
		h_[4] += e;
		h_[5] += f;
		h_[6] += g;
		h_[7] += h;
	}

	uint64_t h_[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
			  0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
	uint8_t buffer_[128] = {};
	size_t used_ = 0;
	uint64_t total_ = 0;
};

} /* namespace */

void sha512(const uint8_t *data, size_t size, uint8_t out[64])
{
	Sha512 hash;
	hash.update(data, size);
	hash.finish(out);
}

void sha512_parts(const uint8_t *const *parts, const size_t *sizes, size_t count, uint8_t out[64])
{
	Sha512 hash;
	for (size_t i = 0; i < count; i++)
		hash.update(parts[i], sizes[i]);
	hash.finish(out);
}

void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t size, uint8_t out[64])
{
	uint8_t block[128] = {};
	if (key_len > sizeof block)
		sha512(key, key_len, block);
	else if (key_len)
		std::memcpy(block, key, key_len);
	uint8_t pad[128];
	for (size_t i = 0; i < sizeof pad; i++)
		pad[i] = block[i] ^ 0x36;
	uint8_t inner[64];
	Sha512 first;
	first.update(pad, sizeof pad);
	first.update(data, size);
	first.finish(inner);
	for (size_t i = 0; i < sizeof pad; i++)
		pad[i] = block[i] ^ 0x5c;
	Sha512 second;
	second.update(pad, sizeof pad);
	second.update(inner, sizeof inner);
	second.finish(out);
}

bool hkdf_sha512(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info,
		 size_t info_len, uint8_t *out, size_t out_len)
{
	if (!out || out_len == 0 || out_len > 64)
		return false;
	uint8_t zeros[64] = {};
	const uint8_t *salt_bytes = salt;
	size_t salt_n = salt_len;
	if (!salt_bytes || salt_n == 0) {
		salt_bytes = zeros;
		salt_n = 64;
	}
	uint8_t prk[64];
	hmac_sha512(salt_bytes, salt_n, ikm ? ikm : zeros, ikm_len, prk);
	std::vector<uint8_t> msg(info_len + 1);
	if (info_len && info)
		std::memcpy(msg.data(), info, info_len);
	msg[info_len] = 1;
	uint8_t block[64];
	hmac_sha512(prk, sizeof prk, msg.data(), msg.size(), block);
	std::memcpy(out, block, out_len);
	return true;
}

bool aead_seal(const uint8_t key[32], uint64_t counter, const uint8_t *aad, size_t aad_len, const uint8_t *plain,
	       size_t plain_len, std::vector<uint8_t> &out)
{
	uint8_t nonce[12];
	nonce_from_counter(counter, nonce);
	return seal_nonce(key, nonce, aad, aad_len, plain, plain_len, out);
}

bool aead_open(const uint8_t key[32], uint64_t counter, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
	       size_t sealed_len, std::vector<uint8_t> &out)
{
	uint8_t nonce[12];
	nonce_from_counter(counter, nonce);
	return open_nonce(key, nonce, aad, aad_len, sealed, sealed_len, out);
}

bool aead_seal_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		     const uint8_t *plain, size_t plain_len, std::vector<uint8_t> &out)
{
	return seal_nonce(key, nonce, aad, aad_len, plain, plain_len, out);
}

bool aead_open_nonce(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		     const uint8_t *sealed, size_t sealed_len, std::vector<uint8_t> &out)
{
	return open_nonce(key, nonce, aad, aad_len, sealed, sealed_len, out);
}

bool aead_self_test()
{
	std::vector<uint8_t> key, nonce, plain, aad, expect, ct, tag, poly_key, poly_msg, poly_tag, block_key,
		block_nonce, block_out;
	if (!unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", block_key) ||
	    !unhex("000000090000004a00000000", block_nonce))
		return false;
	uint8_t block[64];
	chacha_xor(block_key.data(), block_nonce.data(), 1, nullptr, block, 64);
	if (!unhex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
		   "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
		   block_out) ||
	    block_out.size() != 64 || std::memcmp(block, block_out.data(), 64) != 0)
		return false;

	/* RFC 8439 §2.5.2. The last two key bytes are f5 1b; the earlier f0 b7
	 * vector was withdrawn. */
	if (!unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", poly_key))
		return false;
	const char *forum = "Cryptographic Forum Research Group";
	uint8_t got_tag[16];
	poly1305_mac(poly_key.data(), reinterpret_cast<const uint8_t *>(forum), std::strlen(forum), got_tag);
	if (!unhex("a8061dc1305136c6c22b8baf0c0127a9", poly_tag) || std::memcmp(got_tag, poly_tag.data(), 16) != 0)
		return false;

	if (!unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key) ||
	    !unhex("070000004041424344454647", nonce) || !unhex("50515253c0c1c2c3c4c5c6c7", aad))
		return false;
	plain.assign(reinterpret_cast<const uint8_t *>("Ladies and Gentlemen of the class of '99: If I could offer you "
						       "only one tip for the future, sunscreen would be it."),
		     reinterpret_cast<const uint8_t *>("Ladies and Gentlemen of the class of '99: If I could offer you "
						       "only one tip for the future, sunscreen would be it.") +
			     114);
	if (!unhex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
		   "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
		   "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
		   "3ff4def08e4b7a9de576d26586cec64b6116"
		   "1ae10b594f09e26a7e902ecbd0600691",
		   expect))
		return false;
	std::vector<uint8_t> sealed;
	if (!seal_nonce(key.data(), nonce.data(), aad.data(), aad.size(), plain.data(), plain.size(), sealed))
		return false;
	if (sealed != expect)
		return false;
	std::vector<uint8_t> opened;
	if (!open_nonce(key.data(), nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), opened))
		return false;
	if (opened != plain)
		return false;

	uint8_t round_key[32];
	for (int i = 0; i < 32; i++)
		round_key[i] = (uint8_t)(i * 7 + 3);
	std::vector<uint8_t> round_ct, round_pt;
	const uint8_t ad[] = {1, 2, 3, 4, 5};
	const uint8_t msg[] = "control-channel";
	if (!aead_seal(round_key, 0, ad, sizeof ad, msg, sizeof msg - 1, round_ct))
		return false;
	if (!aead_open(round_key, 0, ad, sizeof ad, round_ct.data(), round_ct.size(), round_pt))
		return false;
	if (round_pt.size() != sizeof msg - 1 || std::memcmp(round_pt.data(), msg, round_pt.size()) != 0)
		return false;
	round_ct[0] ^= 1;
	if (aead_open(round_key, 0, ad, sizeof ad, round_ct.data(), round_ct.size(), round_pt))
		return false;
	return true;
}

} /* namespace airplay */
