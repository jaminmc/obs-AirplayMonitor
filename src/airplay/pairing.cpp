/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pair-setup is SRP-6a on the RFC 5054 3072-bit group with SHA-512.
 * H(g) is the single byte 0x05. The session key hashes S with leading zeros
 * removed. The wire value A is still padded to 384 bytes.
 */

#include "airplay/pairing.hpp"

#include "airplay/aead.hpp"
#include "airplay/portable.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <filesystem>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace airplay {
namespace {

constexpr int kLimbs = 48;
constexpr size_t kGroupBytes = 384;
constexpr size_t kMaxFrame = 1024;

const char kIdentity[] = "Pair-Setup";
const char kGroupHex[] =
	"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

void wipe(void *data, size_t size)
{
	if (!data || !size)
		return;
	volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
	while (size--)
		*bytes++ = 0;
}

void wipe(std::vector<uint8_t> &bytes)
{
	if (!bytes.empty())
		wipe(bytes.data(), bytes.size());
	bytes.clear();
}

struct Fe {
	uint64_t v[kLimbs];
};

int fe_cmp(const Fe &a, const Fe &b)
{
	for (int i = kLimbs - 1; i >= 0; i--) {
		if (a.v[i] < b.v[i])
			return -1;
		if (a.v[i] > b.v[i])
			return 1;
	}
	return 0;
}

bool fe_is_zero(const Fe &a)
{
	for (int i = 0; i < kLimbs; i++) {
		if (a.v[i])
			return false;
	}
	return true;
}

void fe_sub(Fe &out, const Fe &a, const Fe &b)
{
	uint64_t borrow = 0;
	for (int i = 0; i < kLimbs; i++) {
		uint64_t diff = a.v[i] - b.v[i];
		uint64_t borrow1 = a.v[i] < b.v[i] ? 1 : 0;
		uint64_t borrow2 = diff < borrow ? 1 : 0;
		out.v[i] = diff - borrow;
		borrow = borrow1 | borrow2;
	}
}

uint64_t fe_add(Fe &out, const Fe &a, const Fe &b)
{
	uint64_t carry = 0;
	for (int i = 0; i < kLimbs; i++)
		out.v[i] = add_carry(a.v[i], b.v[i], carry, carry);
	return carry;
}

void fe_from_be(Fe &out, const uint8_t *be, size_t len)
{
	std::memset(out.v, 0, sizeof out.v);
	if (len > kGroupBytes)
		len = kGroupBytes;
	for (size_t i = 0; i < len; i++) {
		size_t from_end = len - 1 - i;
		out.v[from_end / 8] |= (uint64_t)be[i] << ((from_end % 8) * 8);
	}
}

void fe_pad(const Fe &in, uint8_t out[kGroupBytes])
{
	for (int limb = 0; limb < kLimbs; limb++) {
		uint64_t value = in.v[kLimbs - 1 - limb];
		size_t off = (size_t)limb * 8;
		for (int byte = 0; byte < 8; byte++)
			out[off + byte] = (uint8_t)(value >> (8 * (7 - byte)));
	}
}

void fe_min(const Fe &in, std::vector<uint8_t> &out)
{
	uint8_t padded[kGroupBytes];
	fe_pad(in, padded);
	size_t start = 0;
	while (start < kGroupBytes && padded[start] == 0)
		start++;
	out.assign(padded + start, padded + kGroupBytes);
}

int fe_bitlen(const Fe &in)
{
	for (int i = kLimbs - 1; i >= 0; i--) {
		if (!in.v[i])
			continue;
		int bit = 63;
		while (bit > 0 && ((in.v[i] >> bit) & 1ull) == 0)
			bit--;
		return i * 64 + bit + 1;
	}
	return 0;
}

bool fe_bit(const Fe &in, int bit)
{
	if (bit < 0)
		return false;
	return ((in.v[bit / 64] >> (bit % 64)) & 1ull) != 0;
}

/* Returns the low word of a*b + add + carry and leaves the high word in carry. */
uint64_t mul_add(uint64_t a, uint64_t b, uint64_t add, uint64_t &carry)
{
	uint64_t hi = 0;
	uint64_t lo = mul64(a, b, hi);
	uint64_t c1 = 0;
	uint64_t c2 = 0;
	lo = add_carry(lo, add, 0, c1);
	lo = add_carry(lo, carry, 0, c2);
	carry = hi + c1 + c2;
	return lo;
}

struct Mont {
	Fe N;
	Fe r2;
	Fe neg_n;
	uint64_t n0inv = 0;
};

Fe mont_mul(const Mont &m, const Fe &x, const Fe &y)
{
	uint64_t acc[kLimbs + 2];
	std::memset(acc, 0, sizeof acc);
	for (int i = 0; i < kLimbs; i++) {
		uint64_t carry = 0;
		for (int j = 0; j < kLimbs; j++)
			acc[j] = mul_add(x.v[j], y.v[i], acc[j], carry);
		uint64_t top = 0;
		acc[kLimbs] = add_carry(acc[kLimbs], carry, 0, top);
		acc[kLimbs + 1] = top;

		uint64_t mword = acc[0] * m.n0inv;
		carry = 0;
		for (int j = 0; j < kLimbs; j++)
			acc[j] = mul_add(mword, m.N.v[j], acc[j], carry);
		acc[kLimbs] = add_carry(acc[kLimbs], carry, 0, top);
		acc[kLimbs + 1] += top;

		for (int j = 0; j <= kLimbs; j++)
			acc[j] = acc[j + 1];
	}

	Fe result;
	std::memcpy(result.v, acc, sizeof result.v);
	uint64_t extra = acc[kLimbs];
	while (extra) {
		uint64_t overflow = fe_add(result, result, m.neg_n);
		extra = extra - 1 + overflow;
		if (extra > 4)
			break;
	}
	while (fe_cmp(result, m.N) >= 0)
		fe_sub(result, result, m.N);
	return result;
}

Fe to_mont(const Mont &m, const Fe &x)
{
	return mont_mul(m, x, m.r2);
}

Fe from_mont(const Mont &m, const Fe &x)
{
	Fe one{};
	one.v[0] = 1;
	return mont_mul(m, x, one);
}

Fe modexp(const Mont &m, const Fe &base, const Fe &exp)
{
	Fe one{};
	one.v[0] = 1;
	Fe result = to_mont(m, one);
	Fe window = to_mont(m, base);
	int bits = fe_bitlen(exp);
	for (int i = bits - 1; i >= 0; i--) {
		result = mont_mul(m, result, result);
		if (fe_bit(exp, i))
			result = mont_mul(m, result, window);
	}
	return from_mont(m, result);
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

bool unhex(const char *text, uint8_t *out, size_t out_len)
{
	size_t n = 0;
	for (size_t i = 0; text[i] && n < out_len;) {
		int hi = hex_nibble(text[i]);
		int lo = text[i + 1] ? hex_nibble(text[i + 1]) : -1;
		if (hi < 0 || lo < 0)
			return false;
		out[n++] = (uint8_t)((hi << 4) | lo);
		i += 2;
	}
	return n == out_len;
}

Mont make_mont()
{
	Mont mont;
	uint8_t bytes[kGroupBytes];
	if (!unhex(kGroupHex, bytes, sizeof bytes))
		return mont;
	fe_from_be(mont.N, bytes, sizeof bytes);
	uint64_t inv = 1;
	for (int i = 0; i < 6; i++)
		inv *= 2 - mont.N.v[0] * inv;
	mont.n0inv = ~inv + 1;
	uint64_t carry = 1;
	for (int i = 0; i < kLimbs; i++)
		mont.neg_n.v[i] = add_carry(~mont.N.v[i], 0, carry, carry);
	mont.r2.v[0] = 1;
	for (int i = 0; i < 6144; i++) {
		uint64_t shifted = 0;
		for (int limb = 0; limb < kLimbs; limb++) {
			uint64_t next = mont.r2.v[limb] >> 63;
			mont.r2.v[limb] = (mont.r2.v[limb] << 1) | shifted;
			shifted = next;
		}
		if (shifted) {
			uint64_t overflow = fe_add(mont.r2, mont.r2, mont.neg_n);
			if (overflow) {
				fe_add(mont.r2, mont.r2, mont.neg_n);
				fe_sub(mont.r2, mont.r2, mont.N);
			}
		} else if (fe_cmp(mont.r2, mont.N) >= 0) {
			fe_sub(mont.r2, mont.r2, mont.N);
		}
	}
	return mont;
}

const Mont &group()
{
	static Mont mont = make_mont();
	return mont;
}

Fe reduce_be(const uint8_t *be, size_t len)
{
	Fe value{};
	fe_from_be(value, be, len);
	const Mont &mont = group();
	if (fe_cmp(value, mont.N) >= 0)
		fe_sub(value, value, mont.N);
	return value;
}

Fe hash_parts_fe(const uint8_t *const *parts, const size_t *sizes, size_t count)
{
	uint8_t dig[64];
	sha512_parts(parts, sizes, count, dig);
	Fe out{};
	fe_from_be(out, dig, sizeof dig);
	wipe(dig, sizeof dig);
	return out;
}

Fe hash_fe(const uint8_t *data, size_t size)
{
	const uint8_t *parts[] = {data};
	size_t sizes[] = {size};
	return hash_parts_fe(parts, sizes, data && size ? 1 : 0);
}

Fe private_x(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len)
{
	uint8_t inner[64];
	const uint8_t *parts[] = {reinterpret_cast<const uint8_t *>(kIdentity), reinterpret_cast<const uint8_t *>(":"),
				  password};
	size_t sizes[] = {sizeof kIdentity - 1, 1, password_len};
	sha512_parts(parts, sizes, 3, inner);
	const uint8_t *outer[] = {salt, inner};
	size_t outer_sizes[] = {salt_len, sizeof inner};
	uint8_t dig[64];
	sha512_parts(outer, outer_sizes, salt_len ? 2 : 1, dig);
	if (!salt_len)
		sha512(inner, sizeof inner, dig);
	Fe out{};
	fe_from_be(out, dig, sizeof dig);
	wipe(inner, sizeof inner);
	wipe(dig, sizeof dig);
	return out;
}

Fe multiplier_k()
{
	const Mont &mont = group();
	uint8_t nbuf[kGroupBytes];
	uint8_t gbuf[kGroupBytes];
	fe_pad(mont.N, nbuf);
	std::memset(gbuf, 0, sizeof gbuf);
	gbuf[kGroupBytes - 1] = 5;
	const uint8_t *parts[] = {nbuf, gbuf};
	size_t sizes[] = {sizeof nbuf, sizeof gbuf};
	return hash_parts_fe(parts, sizes, 2);
}

Fe generator()
{
	Fe g{};
	g.v[0] = 5;
	return g;
}

void hash_digest(const uint8_t *const *parts, const size_t *sizes, size_t count, uint8_t out[64])
{
	sha512_parts(parts, sizes, count, out);
}

std::vector<uint8_t> session_key_of(const Fe &S)
{
	std::vector<uint8_t> minimal;
	fe_min(S, minimal);
	uint8_t dig[64];
	sha512(minimal.data(), minimal.size(), dig);
	wipe(minimal);
	std::vector<uint8_t> key(dig, dig + 64);
	wipe(dig, sizeof dig);
	return key;
}

std::vector<uint8_t> client_proof_of(const Fe &A, const Fe &B, const uint8_t *salt, size_t salt_len,
				     const std::vector<uint8_t> &key)
{
	const Mont &mont = group();
	uint8_t nbuf[kGroupBytes];
	fe_pad(mont.N, nbuf);
	uint8_t hn[64];
	uint8_t hg[64];
	uint8_t hi[64];
	sha512(nbuf, sizeof nbuf, hn);
	uint8_t g = 5;
	sha512(&g, 1, hg);
	uint8_t mixed[64];
	for (int i = 0; i < 64; i++)
		mixed[i] = (uint8_t)(hn[i] ^ hg[i]);
	sha512(reinterpret_cast<const uint8_t *>(kIdentity), sizeof kIdentity - 1, hi);
	std::vector<uint8_t> a_bytes;
	std::vector<uint8_t> b_bytes;
	fe_min(A, a_bytes);
	fe_min(B, b_bytes);
	const uint8_t *parts[] = {mixed, hi, salt, a_bytes.data(), b_bytes.data(), key.data()};
	size_t sizes[] = {sizeof mixed, sizeof hi, salt_len, a_bytes.size(), b_bytes.size(), key.size()};
	uint8_t dig[64];
	hash_digest(parts, sizes, 6, dig);
	std::vector<uint8_t> proof(dig, dig + 64);
	wipe(dig, sizeof dig);
	wipe(hn, sizeof hn);
	wipe(hg, sizeof hg);
	wipe(hi, sizeof hi);
	wipe(mixed, sizeof mixed);
	return proof;
}

Fe scrambler_u(const Fe &A, const Fe &B)
{
	uint8_t buf[kGroupBytes * 2];
	fe_pad(A, buf);
	fe_pad(B, buf + kGroupBytes);
	Fe out = hash_fe(buf, sizeof buf);
	wipe(buf, sizeof buf);
	return out;
}

Fe product_mod(const Fe &left, const Fe &right)
{
	const Mont &mont = group();
	return from_mont(mont, mont_mul(mont, to_mont(mont, left), to_mont(mont, right)));
}

} /* namespace */

struct SrpClient::State {
	uint8_t secret[32] = {};
	Fe A{};
	std::vector<uint8_t> password;
	std::vector<uint8_t> key;
	std::vector<uint8_t> proof;
	bool started = false;
};

SrpClient::SrpClient() : state_(new State) {}

SrpClient::~SrpClient()
{
	if (!state_)
		return;
	wipe(state_->secret, sizeof state_->secret);
	wipe(&state_->A, sizeof state_->A);
	wipe(state_->password);
	wipe(state_->key);
	wipe(state_->proof);
	delete state_;
	state_ = nullptr;
}

bool SrpClient::start(const std::string &password, std::string &error)
{
	wipe(state_->secret, sizeof state_->secret);
	wipe(state_->password);
	wipe(state_->key);
	wipe(state_->proof);
	state_->started = false;
	random_bytes(state_->secret, sizeof state_->secret);
	state_->password.assign(password.begin(), password.end());
	Fe exponent{};
	fe_from_be(exponent, state_->secret, sizeof state_->secret);
	state_->A = modexp(group(), generator(), exponent);
	wipe(&exponent, sizeof exponent);
	state_->started = true;
	error.clear();
	return true;
}

std::vector<uint8_t> SrpClient::public_key() const
{
	std::vector<uint8_t> out(kGroupBytes);
	if (state_->started)
		fe_pad(state_->A, out.data());
	else
		out.clear();
	return out;
}

bool SrpClient::proof(const std::vector<uint8_t> &salt, const std::vector<uint8_t> &server_pub,
		      std::vector<uint8_t> &client_proof, std::string &error)
{
	client_proof.clear();
	if (!state_->started) {
		error = "Pairing failed.";
		return false;
	}
	if (server_pub.empty() || server_pub.size() > kGroupBytes) {
		error = "Pairing failed.";
		return false;
	}
	Fe B = reduce_be(server_pub.data(), server_pub.size());
	if (fe_is_zero(B)) {
		error = "Pairing failed.";
		return false;
	}
	Fe u = scrambler_u(state_->A, B);
	if (fe_is_zero(u)) {
		error = "Pairing failed.";
		return false;
	}
	Fe x = private_x(state_->password.data(), state_->password.size(), salt.data(), salt.size());
	wipe(state_->password);
	const Mont &mont = group();
	Fe gx = modexp(mont, generator(), x);
	Fe k = multiplier_k();
	Fe kgx = product_mod(k, gx);
	Fe base;
	if (fe_cmp(B, kgx) >= 0) {
		fe_sub(base, B, kgx);
	} else {
		/* B+N can cross 2^3072. The truncated sum's borrow puts that bit back. */
		Fe sum;
		fe_add(sum, B, mont.N);
		fe_sub(base, sum, kgx);
	}
	/* u*x is the integer exponent, not u*x mod N. */
	Fe ux_full{};
	for (int i = 0; i < 16; i++) {
		if (!u.v[i])
			continue;
		uint64_t carry = 0;
		for (int j = 0; j < 16; j++)
			ux_full.v[i + j] = mul_add(u.v[i], x.v[j], ux_full.v[i + j], carry);
		if (i + 16 < kLimbs)
			ux_full.v[i + 16] = carry;
	}
	Fe exponent{};
	fe_from_be(exponent, state_->secret, sizeof state_->secret);
	fe_add(exponent, exponent, ux_full);
	Fe S = modexp(mont, base, exponent);
	state_->key = session_key_of(S);
	state_->proof = client_proof_of(state_->A, B, salt.data(), salt.size(), state_->key);
	client_proof = state_->proof;
	wipe(&x, sizeof x);
	wipe(&ux_full, sizeof ux_full);
	wipe(&exponent, sizeof exponent);
	wipe(&S, sizeof S);
	wipe(&base, sizeof base);
	error.clear();
	return true;
}

bool SrpClient::verify_server(const std::vector<uint8_t> &server_proof) const
{
	if (state_->key.empty() || state_->proof.empty())
		return false;
	std::vector<uint8_t> a_bytes;
	fe_min(state_->A, a_bytes);
	const uint8_t *parts[] = {a_bytes.data(), state_->proof.data(), state_->key.data()};
	size_t sizes[] = {a_bytes.size(), state_->proof.size(), state_->key.size()};
	uint8_t dig[64];
	sha512_parts(parts, sizes, 3, dig);
	uint8_t diff = server_proof.size() == sizeof dig ? 0 : 1;
	if (!diff) {
		for (size_t i = 0; i < sizeof dig; i++)
			diff |= (uint8_t)(dig[i] ^ server_proof[i]);
	}
	wipe(dig, sizeof dig);
	return diff == 0;
}

const std::vector<uint8_t> &SrpClient::session_key() const
{
	return state_->key;
}

namespace {

Fe server_public_fe(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
		    const uint8_t server_secret[32], Fe &verifier, Fe &secret_fe)
{
	const Mont &mont = group();
	Fe x = private_x(password, password_len, salt, salt_len);
	verifier = modexp(mont, generator(), x);
	wipe(&x, sizeof x);
	fe_from_be(secret_fe, server_secret, 32);
	Fe gb = modexp(mont, generator(), secret_fe);
	Fe k = multiplier_k();
	Fe kv = product_mod(k, verifier);
	Fe sum;
	uint64_t overflow = fe_add(sum, kv, gb);
	if (overflow || fe_cmp(sum, mont.N) >= 0) {
		if (overflow) {
			fe_add(sum, sum, mont.neg_n);
		}
		if (fe_cmp(sum, mont.N) >= 0)
			fe_sub(sum, sum, mont.N);
	}
	wipe(&k, sizeof k);
	return sum;
}

} /* namespace */

bool srp_server_public(const std::string &password, const std::vector<uint8_t> &salt, const uint8_t server_secret[32],
		       std::vector<uint8_t> &server_pub, std::string &error)
{
	if (!server_secret) {
		error = "Pairing failed.";
		return false;
	}
	Fe verifier;
	Fe secret;
	Fe pub = server_public_fe(reinterpret_cast<const uint8_t *>(password.data()), password.size(), salt.data(),
				  salt.size(), server_secret, verifier, secret);
	server_pub.assign(kGroupBytes, 0);
	fe_pad(pub, server_pub.data());
	wipe(&verifier, sizeof verifier);
	wipe(&secret, sizeof secret);
	wipe(&pub, sizeof pub);
	error.clear();
	return true;
}

bool srp_server_proof(const std::string &password, const std::vector<uint8_t> &salt, const uint8_t server_secret[32],
		      const std::vector<uint8_t> &client_pub, const std::vector<uint8_t> &client_proof,
		      std::vector<uint8_t> &server_proof, std::vector<uint8_t> &session_key, std::string &error)
{
	server_proof.clear();
	session_key.clear();
	if (!server_secret || client_pub.empty() || client_pub.size() > kGroupBytes) {
		error = "Pairing failed.";
		return false;
	}
	Fe verifier;
	Fe secret;
	Fe B = server_public_fe(reinterpret_cast<const uint8_t *>(password.data()), password.size(), salt.data(),
				salt.size(), server_secret, verifier, secret);
	Fe A = reduce_be(client_pub.data(), client_pub.size());
	if (fe_is_zero(A) || fe_is_zero(B)) {
		error = "Pairing failed.";
		return false;
	}
	Fe u = scrambler_u(A, B);
	const Mont &mont = group();
	Fe vu = modexp(mont, verifier, u);
	Fe base = product_mod(A, vu);
	Fe S = modexp(mont, base, secret);
	session_key = session_key_of(S);
	std::vector<uint8_t> expect = client_proof_of(A, B, salt.data(), salt.size(), session_key);
	uint8_t diff = expect.size() == client_proof.size() ? 0 : 1;
	if (!diff) {
		for (size_t i = 0; i < expect.size(); i++)
			diff |= (uint8_t)(expect[i] ^ client_proof[i]);
	}
	if (diff) {
		wipe(session_key);
		wipe(expect);
		error = "Pairing failed.";
		return false;
	}
	std::vector<uint8_t> a_bytes;
	fe_min(A, a_bytes);
	const uint8_t *parts[] = {a_bytes.data(), client_proof.data(), session_key.data()};
	size_t sizes[] = {a_bytes.size(), client_proof.size(), session_key.size()};
	uint8_t dig[64];
	sha512_parts(parts, sizes, 3, dig);
	server_proof.assign(dig, dig + 64);
	wipe(dig, sizeof dig);
	wipe(&S, sizeof S);
	wipe(&secret, sizeof secret);
	wipe(&verifier, sizeof verifier);
	error.clear();
	return true;
}

std::vector<uint8_t> tlv_encode(const std::vector<TlvItem> &items)
{
	std::vector<uint8_t> out;
	for (const TlvItem &item : items) {
		size_t offset = 0;
		if (item.value.empty()) {
			out.push_back(item.type);
			out.push_back(0);
			continue;
		}
		while (offset < item.value.size()) {
			size_t n = item.value.size() - offset;
			if (n > 255)
				n = 255;
			out.push_back(item.type);
			out.push_back((uint8_t)n);
			out.insert(out.end(), item.value.begin() + (std::ptrdiff_t)offset,
				   item.value.begin() + (std::ptrdiff_t)(offset + n));
			offset += n;
		}
	}
	return out;
}

bool tlv_get(const std::vector<uint8_t> &message, uint8_t type, std::vector<uint8_t> &value)
{
	value.clear();
	bool found = false;
	size_t i = 0;
	while (i + 2 <= message.size()) {
		uint8_t item = message[i];
		size_t n = message[i + 1];
		if (i + 2 + n > message.size())
			return false;
		if (item == type) {
			found = true;
			value.insert(value.end(), message.begin() + (std::ptrdiff_t)(i + 2),
				     message.begin() + (std::ptrdiff_t)(i + 2 + n));
		}
		i += 2 + n;
	}
	return found && i == message.size();
}

std::vector<uint8_t> pair_setup_m1()
{
	TlvItem method;
	method.type = (uint8_t)TlvType::Method;
	method.value.push_back(0);
	TlvItem state;
	state.type = (uint8_t)TlvType::State;
	state.value.push_back(1);
	TlvItem flags;
	flags.type = (uint8_t)TlvType::Flags;
	flags.value.push_back(kTlvFlagTransient);
	return tlv_encode({method, state, flags});
}

std::vector<uint8_t> pair_setup_m1_pin()
{
	TlvItem method;
	method.type = (uint8_t)TlvType::Method;
	method.value.push_back(0);
	TlvItem state;
	state.type = (uint8_t)TlvType::State;
	state.value.push_back(1);
	return tlv_encode({method, state});
}

struct SealedControl::Impl {
	ByteTransport &inner;
	uint8_t write_key[32] = {};
	uint8_t read_key[32] = {};
	uint64_t write_counter = 0;
	uint64_t read_counter = 0;
	std::vector<uint8_t> raw;
	std::vector<uint8_t> plain;
	size_t plain_offset = 0;
};

SealedControl::SealedControl(ByteTransport &inner, const uint8_t write_key[32], const uint8_t read_key[32])
	: impl_(new Impl{inner})
{
	std::memcpy(impl_->write_key, write_key, 32);
	std::memcpy(impl_->read_key, read_key, 32);
}

SealedControl::SealedControl(ByteTransport &inner, const std::vector<uint8_t> &secret, bool swap_directions)
	: impl_(new Impl{inner})
{
	const char *salt = "Control-Salt";
	const char *write_info = swap_directions ? "Control-Read-Encryption-Key" : "Control-Write-Encryption-Key";
	const char *read_info = swap_directions ? "Control-Write-Encryption-Key" : "Control-Read-Encryption-Key";
	hkdf_sha512(secret.data(), secret.size(), reinterpret_cast<const uint8_t *>(salt), std::strlen(salt),
		    reinterpret_cast<const uint8_t *>(write_info), std::strlen(write_info), impl_->write_key, 32);
	hkdf_sha512(secret.data(), secret.size(), reinterpret_cast<const uint8_t *>(salt), std::strlen(salt),
		    reinterpret_cast<const uint8_t *>(read_info), std::strlen(read_info), impl_->read_key, 32);
}

SealedControl::~SealedControl()
{
	if (!impl_)
		return;
	wipe(impl_->write_key, sizeof impl_->write_key);
	wipe(impl_->read_key, sizeof impl_->read_key);
	wipe(impl_->raw);
	wipe(impl_->plain);
	delete impl_;
	impl_ = nullptr;
}

bool SealedControl::write_all(const uint8_t *data, size_t size)
{
	size_t offset = 0;
	while (offset < size) {
		size_t n = size - offset;
		if (n > kMaxFrame)
			n = kMaxFrame;
		uint8_t prefix[2] = {(uint8_t)n, (uint8_t)(n >> 8)};
		std::vector<uint8_t> sealed;
		if (!aead_seal(impl_->write_key, impl_->write_counter, prefix, sizeof prefix, data + offset, n, sealed))
			return false;
		impl_->write_counter++;
		uint8_t frame[2 + kMaxFrame + 16];
		frame[0] = prefix[0];
		frame[1] = prefix[1];
		std::memcpy(frame + 2, sealed.data(), sealed.size());
		if (!impl_->inner.write_all(frame, 2 + sealed.size()))
			return false;
		offset += n;
	}
	return true;
}

int SealedControl::read_some(uint8_t *data, size_t capacity, int timeout_ms)
{
	if (!data || capacity == 0)
		return 0;
	while (impl_->plain_offset >= impl_->plain.size()) {
		impl_->plain.clear();
		impl_->plain_offset = 0;
		while (impl_->raw.size() < 2) {
			uint8_t chunk[2048];
			int count = impl_->inner.read_some(chunk, sizeof chunk, timeout_ms);
			if (count <= 0)
				return count;
			impl_->raw.insert(impl_->raw.end(), chunk, chunk + count);
		}
		size_t plain_len = (size_t)impl_->raw[0] | ((size_t)impl_->raw[1] << 8);
		if (plain_len > kMaxFrame)
			return -1;
		size_t frame = 2 + plain_len + 16;
		while (impl_->raw.size() < frame) {
			uint8_t chunk[2048];
			int count = impl_->inner.read_some(chunk, sizeof chunk, timeout_ms);
			if (count < 0)
				return -1;
			if (count == 0)
				return 0;
			impl_->raw.insert(impl_->raw.end(), chunk, chunk + count);
		}
		uint8_t prefix[2] = {impl_->raw[0], impl_->raw[1]};
		if (!aead_open(impl_->read_key, impl_->read_counter, prefix, sizeof prefix, impl_->raw.data() + 2,
			       plain_len + 16, impl_->plain))
			return -1;
		impl_->read_counter++;
		impl_->raw.erase(impl_->raw.begin(), impl_->raw.begin() + (std::ptrdiff_t)frame);
	}
	size_t available = impl_->plain.size() - impl_->plain_offset;
	size_t n = available < capacity ? available : capacity;
	std::memcpy(data, impl_->plain.data() + impl_->plain_offset, n);
	impl_->plain_offset += n;
	if (impl_->plain_offset >= impl_->plain.size()) {
		wipe(impl_->plain);
		impl_->plain_offset = 0;
	}
	return (int)n;
}

namespace {

bool expect_fe(const Fe &got, uint64_t value, const char *label)
{
	if (got.v[0] == value) {
		for (int i = 1; i < kLimbs; i++) {
			if (got.v[i]) {
				std::fprintf(stderr, "srp: %s limb %d\n", label, i);
				return false;
			}
		}
		return true;
	}
	std::fprintf(stderr, "srp: %s got %llu\n", label, (unsigned long long)got.v[0]);
	return false;
}

} /* namespace */

extern "C" {
int crypto_sign_keypair(uint8_t *pk, uint8_t *sk);
int crypto_sign(uint8_t *sm, unsigned long long *smlen, const uint8_t *m, unsigned long long n, const uint8_t *sk);
int crypto_sign_open(uint8_t *m, unsigned long long *mlen, const uint8_t *sm, unsigned long long n, const uint8_t *pk);
int crypto_scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p);
int crypto_scalarmult_base(uint8_t *q, const uint8_t *n);
}

void secure_clear(void *data, size_t size)
{
	wipe(data, size);
}

void secure_clear(std::vector<uint8_t> &bytes)
{
	wipe(bytes);
}

namespace {

const uint8_t kScreenAcl[] = {0xe1, 0x57, 'c', 'o', 'm', '.', 'a', 'p', 'p', 'l', 'e', '.', 'S',
			      'c',  'r',  'e', 'e', 'n', 'C', 'a', 'p', 't', 'u', 'r', 'e', 0x01};

bool hap_nonce(const char *label, uint8_t nonce[12])
{
	std::memset(nonce, 0, 12);
	if (!label || std::strlen(label) != 8)
		return false;
	std::memcpy(nonce + 4, label, 8);
	return true;
}

bool hkdf32(const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info, uint8_t out[32])
{
	return hkdf_sha512(ikm, ikm_len, reinterpret_cast<const uint8_t *>(salt), std::strlen(salt),
			   reinterpret_cast<const uint8_t *>(info), std::strlen(info), out, 32);
}

bool ed25519_sign(const PairingIdentity &identity, const std::vector<uint8_t> &message, std::vector<uint8_t> &signature)
{
	signature.clear();
	if (message.empty())
		return false;
	std::vector<uint8_t> signed_message(message.size() + 64);
	unsigned long long length = 0;
	int rc = crypto_sign(signed_message.data(), &length, message.data(), message.size(), identity.private_key);
	if (rc != 0 || length < 64) {
		secure_clear(signed_message);
		return false;
	}
	signature.assign(signed_message.begin(), signed_message.begin() + 64);
	secure_clear(signed_message);
	return signature.size() == 64;
}

} /* namespace */

PairingIdentity::~PairingIdentity()
{
	wipe(private_key, sizeof private_key);
	wipe(public_key, sizeof public_key);
}

VerifyEphemeral::~VerifyEphemeral()
{
	wipe(secret, sizeof secret);
	wipe(public_key, sizeof public_key);
}

bool pairing_identity_generate(PairingIdentity &identity, const std::string &id)
{
	identity.id = id;
	wipe(identity.private_key, sizeof identity.private_key);
	wipe(identity.public_key, sizeof identity.public_key);
	return !id.empty() && crypto_sign_keypair(identity.public_key, identity.private_key) == 0;
}

namespace {

std::string pairing_directory()
{
	const char *override_dir = std::getenv("AIRPLAY_PAIRING_DIR");
	if (override_dir && override_dir[0])
		return override_dir;
#if defined(_WIN32)
	const char *base = std::getenv("APPDATA");
	if (!base || !base[0])
		return {};
	return std::string(base) + "/obs-airplay-monitor";
#else
	const char *home = std::getenv("HOME");
#if defined(__APPLE__)
	if (!home || !home[0])
		return {};
	return std::string(home) + "/Library/Application Support/obs-airplay-monitor";
#else
	const char *config = std::getenv("XDG_CONFIG_HOME");
	if (config && config[0])
		return std::string(config) + "/obs-airplay-monitor";
	if (!home || !home[0])
		return {};
	return std::string(home) + "/.config/obs-airplay-monitor";
#endif
#endif
}

bool ensure_dir(const std::string &path)
{
	if (path.empty())
		return false;
	std::error_code error;
	std::filesystem::create_directories(path, error);
	if (!std::filesystem::is_directory(path, error))
		return false;
	std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
	return true;
}

/* Writes a file only the owner can read. On POSIX it is created with that mode, never wider. */
bool write_private_file(const std::string &path, const uint8_t *data, size_t size)
{
#if defined(_WIN32)
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (size)
		out.write(reinterpret_cast<const char *>(data), (std::streamsize)size);
	return (bool)out;
#else
	int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return false;
	size_t done = 0;
	while (done < size) {
		ssize_t wrote = write(fd, data + done, size - done);
		if (wrote <= 0)
			break;
		done += (size_t)wrote;
	}
	close(fd);
	return done == size;
#endif
}

std::string device_filename(const std::string &device_id)
{
	std::string name;
	for (unsigned char ch : device_id) {
		if (ch == ':' || ch == '-' || ch == ' ')
			continue;
		if (std::isxdigit(ch))
			name.push_back((char)std::toupper(ch));
		else if (std::isalnum(ch))
			name.push_back((char)ch);
	}
	if (name.empty())
		name = "receiver";
	return name;
}

std::string new_controller_id()
{
	uint8_t bytes[16];
	random_bytes(bytes, sizeof bytes);
	bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
	bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
	char text[40];
	std::snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
		      bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
	return text;
}

bool write_identity_file(const std::string &path, const PairingIdentity &identity)
{
	if (identity.id.size() > 80)
		return false;
	std::vector<uint8_t> bytes = {'A', 'P', 'M', '1', (uint8_t)identity.id.size()};
	bytes.insert(bytes.end(), identity.id.begin(), identity.id.end());
	bytes.insert(bytes.end(), identity.private_key, identity.private_key + sizeof identity.private_key);
	std::string temp = path + ".tmp";
	bool ok = write_private_file(temp, bytes.data(), bytes.size());
	wipe(bytes);
	std::error_code error;
	if (ok)
		std::filesystem::rename(temp, path, error);
	if (!ok || error) {
		std::filesystem::remove(temp, error);
		return false;
	}
	return true;
}

bool read_identity_file(const std::string &path, PairingIdentity &identity)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	char magic[4];
	in.read(magic, 4);
	unsigned char id_len = 0;
	in.read(reinterpret_cast<char *>(&id_len), 1);
	if (!in || std::memcmp(magic, "APM1", 4) != 0 || id_len == 0 || id_len > 80)
		return false;
	std::string id(id_len, '\0');
	in.read(id.data(), id_len);
	uint8_t secret[64];
	in.read(reinterpret_cast<char *>(secret), 64);
	if (!in)
		return false;
	identity.id = std::move(id);
	std::memcpy(identity.private_key, secret, 64);
	std::memcpy(identity.public_key, secret + 32, 32);
	wipe(secret, sizeof secret);
	return true;
}

} /* namespace */

bool pairing_identity_persistent(PairingIdentity &identity)
{
	std::string dir = pairing_directory();
	if (!ensure_dir(dir))
		return pairing_identity_generate(identity, new_controller_id());
	std::string path = dir + "/controller";
	if (read_identity_file(path, identity))
		return !identity.id.empty();
	if (!pairing_identity_generate(identity, new_controller_id()))
		return false;
	return write_identity_file(path, identity);
}

bool pairing_device_remembered(const std::string &device_id)
{
	std::string dir = pairing_directory();
	if (dir.empty())
		return false;
	std::string path = dir + "/paired/" + device_filename(device_id);
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

void pairing_device_remember(const std::string &device_id)
{
	std::string dir = pairing_directory();
	if (!ensure_dir(dir) || !ensure_dir(dir + "/paired"))
		return;
	std::string path = dir + "/paired/" + device_filename(device_id);
	write_private_file(path, nullptr, 0);
}

void pairing_device_forget(const std::string &device_id)
{
	std::string dir = pairing_directory();
	if (dir.empty())
		return;
	std::string path = dir + "/paired/" + device_filename(device_id);
	std::error_code error;
	std::filesystem::remove(path, error);
}

bool pair_setup_m5(const PairingIdentity &identity, const std::vector<uint8_t> &session_key, bool screen_capture,
		   std::vector<uint8_t> &body)
{
	body.clear();
	if (session_key.empty() || identity.id.empty())
		return false;
	uint8_t enc_key[32];
	uint8_t sig_key[32];
	if (!hkdf32(session_key.data(), session_key.size(), "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info",
		    enc_key) ||
	    !hkdf32(session_key.data(), session_key.size(), "Pair-Setup-Controller-Sign-Salt",
		    "Pair-Setup-Controller-Sign-Info", sig_key)) {
		wipe(enc_key, sizeof enc_key);
		wipe(sig_key, sizeof sig_key);
		return false;
	}
	std::vector<uint8_t> preimage;
	preimage.insert(preimage.end(), sig_key, sig_key + sizeof sig_key);
	preimage.insert(preimage.end(), identity.id.begin(), identity.id.end());
	preimage.insert(preimage.end(), identity.public_key, identity.public_key + sizeof identity.public_key);
	std::vector<uint8_t> signature;
	bool signed_ok = ed25519_sign(identity, preimage, signature);
	secure_clear(preimage);
	wipe(sig_key, sizeof sig_key);
	if (!signed_ok) {
		wipe(enc_key, sizeof enc_key);
		return false;
	}
	TlvItem identifier;
	identifier.type = (uint8_t)TlvType::Identifier;
	identifier.value.assign(identity.id.begin(), identity.id.end());
	TlvItem public_key;
	public_key.type = (uint8_t)TlvType::PublicKey;
	public_key.value.assign(identity.public_key, identity.public_key + sizeof identity.public_key);
	TlvItem signature_item;
	signature_item.type = (uint8_t)TlvType::Signature;
	signature_item.value = std::move(signature);
	std::vector<TlvItem> sub{identifier, public_key, signature_item};
	if (screen_capture) {
		TlvItem acl;
		acl.type = (uint8_t)TlvType::Acl;
		acl.value.assign(kScreenAcl, kScreenAcl + sizeof kScreenAcl);
		sub.push_back(std::move(acl));
	}
	std::vector<uint8_t> plain = tlv_encode(sub);
	uint8_t nonce[12];
	std::vector<uint8_t> sealed;
	bool sealed_ok = hap_nonce("PS-Msg05", nonce) &&
			 aead_seal_nonce(enc_key, nonce, nullptr, 0, plain.data(), plain.size(), sealed);
	wipe(enc_key, sizeof enc_key);
	secure_clear(plain);
	if (!sealed_ok) {
		secure_clear(sealed);
		return false;
	}
	TlvItem encrypted;
	encrypted.type = (uint8_t)TlvType::EncryptedData;
	encrypted.value = std::move(sealed);
	TlvItem state;
	state.type = (uint8_t)TlvType::State;
	state.value.push_back(5);
	body = tlv_encode({encrypted, state});
	return !body.empty();
}

bool pair_verify_m1(VerifyEphemeral &ephemeral, std::vector<uint8_t> &body)
{
	body.clear();
	random_bytes(ephemeral.secret, sizeof ephemeral.secret);
	if (crypto_scalarmult_base(ephemeral.public_key, ephemeral.secret) != 0)
		return false;
	TlvItem state;
	state.type = (uint8_t)TlvType::State;
	state.value.push_back(1);
	TlvItem public_key;
	public_key.type = (uint8_t)TlvType::PublicKey;
	public_key.value.assign(ephemeral.public_key, ephemeral.public_key + sizeof ephemeral.public_key);
	body = tlv_encode({state, public_key});
	return body.size() == 2 + 1 + 2 + 32;
}

bool pair_verify_finish(const PairingIdentity &identity, VerifyEphemeral &ephemeral,
			const std::vector<uint8_t> &server_public, const std::vector<uint8_t> *encrypted,
			std::vector<uint8_t> &shared, std::vector<uint8_t> &v3_body)
{
	shared.clear();
	v3_body.clear();
	if (server_public.size() < 32 || identity.id.empty())
		return false;
	uint8_t raw[32];
	if (crypto_scalarmult(raw, ephemeral.secret, server_public.data()) != 0) {
		wipe(raw, sizeof raw);
		return false;
	}
	uint8_t verify_key[32];
	if (!hkdf32(raw, sizeof raw, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", verify_key)) {
		wipe(raw, sizeof raw);
		wipe(verify_key, sizeof verify_key);
		return false;
	}
	if (encrypted && !encrypted->empty()) {
		uint8_t nonce[12];
		std::vector<uint8_t> opened;
		bool opened_ok =
			hap_nonce("PV-Msg02", nonce) &&
			aead_open_nonce(verify_key, nonce, nullptr, 0, encrypted->data(), encrypted->size(), opened);
		secure_clear(opened);
		if (!opened_ok) {
			wipe(raw, sizeof raw);
			wipe(verify_key, sizeof verify_key);
			return false;
		}
	}
	std::vector<uint8_t> preimage;
	preimage.insert(preimage.end(), ephemeral.public_key, ephemeral.public_key + sizeof ephemeral.public_key);
	preimage.insert(preimage.end(), identity.id.begin(), identity.id.end());
	preimage.insert(preimage.end(), server_public.begin(), server_public.begin() + 32);
	std::vector<uint8_t> signature;
	bool signed_ok = ed25519_sign(identity, preimage, signature);
	secure_clear(preimage);
	if (!signed_ok) {
		wipe(raw, sizeof raw);
		wipe(verify_key, sizeof verify_key);
		return false;
	}
	TlvItem identifier;
	identifier.type = (uint8_t)TlvType::Identifier;
	identifier.value.assign(identity.id.begin(), identity.id.end());
	TlvItem signature_item;
	signature_item.type = (uint8_t)TlvType::Signature;
	signature_item.value = std::move(signature);
	std::vector<uint8_t> plain = tlv_encode({identifier, signature_item});
	uint8_t nonce[12];
	std::vector<uint8_t> sealed;
	bool sealed_ok = hap_nonce("PV-Msg03", nonce) &&
			 aead_seal_nonce(verify_key, nonce, nullptr, 0, plain.data(), plain.size(), sealed);
	wipe(verify_key, sizeof verify_key);
	secure_clear(plain);
	if (!sealed_ok) {
		secure_clear(sealed);
		wipe(raw, sizeof raw);
		return false;
	}
	TlvItem state;
	state.type = (uint8_t)TlvType::State;
	state.value.push_back(3);
	TlvItem encrypted_item;
	encrypted_item.type = (uint8_t)TlvType::EncryptedData;
	encrypted_item.value = std::move(sealed);
	v3_body = tlv_encode({state, encrypted_item});
	shared.assign(raw, raw + sizeof raw);
	wipe(raw, sizeof raw);
	return !v3_body.empty();
}

bool curve_self_test()
{
	uint8_t scalar[32];
	uint8_t point[32];
	uint8_t expect[32];
	uint8_t got[32];
	if (!unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, sizeof scalar) ||
	    !unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, sizeof point) ||
	    !unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", expect, sizeof expect))
		return false;
	if (crypto_scalarmult(got, scalar, point) != 0 || std::memcmp(got, expect, sizeof got) != 0) {
		std::fprintf(stderr, "curve: x25519 vector\n");
		return false;
	}
	uint8_t nine[32] = {9};
	uint8_t base[32];
	uint8_t via[32];
	random_bytes(scalar, sizeof scalar);
	if (crypto_scalarmult_base(base, scalar) != 0 || crypto_scalarmult(via, scalar, nine) != 0 ||
	    std::memcmp(base, via, sizeof base) != 0) {
		std::fprintf(stderr, "curve: x25519 base\n");
		return false;
	}

	PairingIdentity identity;
	if (!pairing_identity_generate(identity, "00000000-0000-4000-8000-000000000001")) {
		std::fprintf(stderr, "curve: keypair\n");
		return false;
	}
	const char *text = "airplay";
	std::vector<uint8_t> message(text, text + 7);
	std::vector<uint8_t> signed_message(message.size() + 64);
	unsigned long long length = 0;
	if (crypto_sign(signed_message.data(), &length, message.data(), message.size(), identity.private_key) != 0) {
		std::fprintf(stderr, "curve: sign\n");
		return false;
	}
	std::vector<uint8_t> opened(length);
	unsigned long long opened_len = 0;
	if (crypto_sign_open(opened.data(), &opened_len, signed_message.data(), length, identity.public_key) != 0 ||
	    opened_len != message.size() || std::memcmp(opened.data(), message.data(), message.size()) != 0) {
		std::fprintf(stderr, "curve: open\n");
		return false;
	}
	signed_message[70] ^= 1;
	if (crypto_sign_open(opened.data(), &opened_len, signed_message.data(), length, identity.public_key) == 0) {
		std::fprintf(stderr, "curve: tamper\n");
		return false;
	}

	uint8_t key[32];
	random_bytes(key, sizeof key);
	uint8_t nonce[12];
	std::vector<uint8_t> sealed;
	std::vector<uint8_t> plain;
	if (!hap_nonce("PS-Msg05", nonce) ||
	    !aead_seal_nonce(key, nonce, nullptr, 0, message.data(), message.size(), sealed) ||
	    !aead_open_nonce(key, nonce, nullptr, 0, sealed.data(), sealed.size(), plain) || plain != message) {
		std::fprintf(stderr, "curve: hap nonce\n");
		return false;
	}
	sealed.back() ^= 1;
	if (aead_open_nonce(key, nonce, nullptr, 0, sealed.data(), sealed.size(), plain)) {
		std::fprintf(stderr, "curve: hap tag\n");
		return false;
	}
	std::vector<uint8_t> pin_m1 = pair_setup_m1_pin();
	if (pin_m1.size() != 6 || pin_m1[0] != 0x00 || pin_m1[1] != 0x01 || pin_m1[2] != 0x00 || pin_m1[3] != 0x06 ||
	    pin_m1[5] != 0x01) {
		std::fprintf(stderr, "curve: pin m1\n");
		return false;
	}
	if (sizeof kScreenAcl != 26) {
		std::fprintf(stderr, "curve: acl\n");
		return false;
	}
	return true;
}

bool srp_self_test()
{
	const Mont &mont = group();
	if ((mont.N.v[0] & 1ull) == 0) {
		std::fprintf(stderr, "srp: N even\n");
		return false;
	}
	uint8_t gbyte = 5;
	uint8_t hg[64];
	sha512(&gbyte, 1, hg);
	uint8_t hg_expect[64];
	if (!unhex("095f3e448a55fa2c3c7049c01dcf12fb87c44477cf367993132ca74c92a27a1eaaa341c2fd5724ea449cec2728547e80edbb7e6029891c5ffd157e283ebb41e5",
		   hg_expect, sizeof hg_expect) ||
	    std::memcmp(hg, hg_expect, sizeof hg) != 0) {
		std::fprintf(stderr, "srp: H(g)\n");
		return false;
	}
	Fe k = multiplier_k();
	std::vector<uint8_t> k_bytes;
	fe_min(k, k_bytes);
	uint8_t k_expect[64];
	if (!unhex("a9c2e2559bf0ebb53f0cbbf62282906bede7f2182f00678211fbd5bde5b285033a4993503b87397f9be5ec02080fedbc0835587ad039060879b8621e8c3659e0",
		   k_expect, sizeof k_expect) ||
	    k_bytes.size() != sizeof k_expect || std::memcmp(k_bytes.data(), k_expect, sizeof k_expect) != 0) {
		std::fprintf(stderr, "srp: k size %zu\n", k_bytes.size());
		return false;
	}

	Fe three{};
	three.v[0] = 3;
	Fe five{};
	five.v[0] = 5;
	if (!expect_fe(product_mod(three, five), 15, "3*5"))
		return false;
	Fe exp5{};
	exp5.v[0] = 5;
	if (!expect_fe(modexp(mont, generator(), exp5), 3125, "5^5"))
		return false;
	Fe nm1;
	Fe one{};
	one.v[0] = 1;
	fe_sub(nm1, mont.N, one);
	Fe two{};
	two.v[0] = 2;
	if (!expect_fe(modexp(mont, nm1, two), 1, "(N-1)^2"))
		return false;

	std::string error;
	SrpClient client;
	if (!client.start("3939", error)) {
		std::fprintf(stderr, "srp: start\n");
		return false;
	}
	std::vector<uint8_t> salt(16, 0x3c);
	uint8_t server_secret[32];
	for (int i = 0; i < 32; i++)
		server_secret[i] = (uint8_t)(0x40 + i);
	std::vector<uint8_t> server_pub;
	if (!srp_server_public("3939", salt, server_secret, server_pub, error)) {
		std::fprintf(stderr, "srp: server public\n");
		return false;
	}
	std::vector<uint8_t> client_proof;
	if (!client.proof(salt, server_pub, client_proof, error)) {
		std::fprintf(stderr, "srp: client proof\n");
		return false;
	}
	std::vector<uint8_t> server_proof;
	std::vector<uint8_t> server_key;
	if (!srp_server_proof("3939", salt, server_secret, client.public_key(), client_proof, server_proof, server_key,
			      error)) {
		std::fprintf(stderr, "srp: server proof\n");
		return false;
	}
	if (server_key != client.session_key() || !client.verify_server(server_proof)) {
		std::fprintf(stderr, "srp: keys differ\n");
		return false;
	}

	std::vector<uint8_t> m1 = pair_setup_m1();
	if (m1.size() != 9 || m1[0] != 0x00 || m1[2] != 0x00 || m1[3] != 0x06 || m1[6] != 0x13 || m1[8] != 0x10)
		return false;
	std::vector<uint8_t> flag;
	if (!tlv_get(m1, (uint8_t)TlvType::Flags, flag) || flag.size() != 1 || flag[0] != kTlvFlagTransient)
		return false;
	if (!curve_self_test())
		return false;
	return true;
}

} /* namespace airplay */
