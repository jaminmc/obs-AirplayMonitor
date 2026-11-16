/* SPDX-License-Identifier: LGPL-3.0-or-later
 * C++ port, made 2026-11-15 for AirPlay Monitor, of the Go packages fpbridge, fpsapcore and
 * fairplayhash from github.com/objevovat/fairplay-sap-core-airplay2-sender-authentication-handshake.
 * fpsapcore and parts of fpbridge derive from github.com/omarroth/doubletake at 8ccea5f.
 * Each function names the Go function it follows. See ../../NOTICE.md.
 */

#include "fairplay_sender.hpp"

#include "../c/fairplay_sapcore.h"

#include <cstring>

namespace fairplay_sap {
namespace {

#include "fairplay_tables.inc"

uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t load_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

void store_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void store_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

uint64_t load_le64(const uint8_t *p)
{
	return (uint64_t)load_le32(p) | (uint64_t)load_le32(p + 4) << 32;
}

void store_le64(uint8_t *p, uint64_t v)
{
	store_le32(p, (uint32_t)v);
	store_le32(p + 4, (uint32_t)(v >> 32));
}

uint32_t rotl32(uint32_t v, unsigned n)
{
	n &= 31u;
	return n ? (v << n) | (v >> (32u - n)) : v;
}

uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t product = 0;
	while (b) {
		if (b & 1)
			product ^= a;
		uint8_t high = a & 0x80;
		a = (uint8_t)(a << 1);
		if (high)
			a ^= 0x1b;
		b >>= 1;
	}
	return product;
}

/* Every packed table is base[v ^ in_xor] ^ out_xor, with spec = (base index, in_xor, out_xor). */
void unpack(uint8_t dst[256], const uint8_t *bases, const uint8_t *spec)
{
	const uint8_t *base = bases + (size_t)spec[0] * 256;
	for (int v = 0; v < 256; v++)
		dst[v] = base[(uint8_t)(v ^ spec[1])] ^ spec[2];
}

struct Tables {
	uint8_t type_i[160][256];
	uint32_t type_ii[4][256];
	uint8_t ground[16][256];
	uint8_t spn1_core[9][16][256];
	uint8_t spn1_trail[16][256];
	uint8_t output_dec[16][256];
	uint64_t spn1_mix_nib[32][16][2];
	uint64_t spn1_mix_const[2];
	uint8_t tail_final[16][256];
	uint8_t inverse_sbox[256];
	uint8_t mul2[256], mul3[256], mul9[256], mul11[256], mul13[256], mul14[256];

	Tables()
	{
		for (int i = 0; i < 160; i++)
			unpack(type_i[i], kTypeIBases, kTypeISpec + i * 3);
		std::memset(type_ii, 0, sizeof type_ii);
		/* wbaes_tables.go buildWBAESTypeII */
		for (int k = 0; k < 4; k++) {
			for (int lane = 0; lane < 4; lane++) {
				uint8_t column[256];
				unpack(column, kTypeIIBases, kTypeIISpec + (k * 4 + lane) * 3);
				for (int v = 0; v < 256; v++)
					type_ii[k][v] |= (uint32_t)column[v] << (8 * lane);
			}
		}
		for (int i = 0; i < 16; i++) {
			unpack(ground[i], kGroundBases, kGroundSpec + i * 3);
			unpack(spn1_trail[i], kSpn1TrailBases, kSpn1TrailSpec + i * 3);
			unpack(output_dec[i], kOutputDecBases, kOutputDecSpec + i * 3);
		}
		for (int r = 0; r < 9; r++)
			for (int j = 0; j < 16; j++)
				unpack(spn1_core[r][j], kSpn1CoreBases, kSpn1CoreSpec + (r * 16 + j) * 3);
		/* tail_spn_fast.go buildTailSPNFinal */
		for (int p = 0; p < 16; p++)
			for (int v = 0; v < 256; v++)
				tail_final[p][v] = type_i[144 + p][v] ^ kTailXorMask[p];
		build_mix_columns();
		for (int v = 0; v < 256; v++) {
			inverse_sbox[kAesSbox[v]] = (uint8_t)v;
			mul2[v] = gf_mul((uint8_t)v, 2);
			mul3[v] = gf_mul((uint8_t)v, 3);
			mul9[v] = gf_mul((uint8_t)v, 9);
			mul11[v] = gf_mul((uint8_t)v, 11);
			mul13[v] = gf_mul((uint8_t)v, 13);
			mul14[v] = gf_mul((uint8_t)v, 14);
		}
	}

	/* spn1_fullmix_const.go buildMixColRows and spn1_mixcol_fast.go init */
	void build_mix_columns()
	{
		uint8_t rows[128][16] = {};
		for (int r = 0; r < 128; r++) {
			int column = r / 32;
			for (int k = 0; k < 4; k++)
				rows[r][column * 4 + k] = kSpn1MixBlock[(r % 32) * 4 + k];
		}
		uint64_t cols[128][2] = {};
		for (int ob = 0; ob < 128; ob++)
			for (int ib = 0; ib < 128; ib++)
				if ((rows[ob][ib >> 3] >> (ib & 7)) & 1)
					cols[ib][ob >> 6] |= (uint64_t)1 << (ob & 63);
		for (int n = 0; n < 32; n++) {
			for (int v = 0; v < 16; v++) {
				uint64_t acc[2] = {0, 0};
				for (int k = 0; k < 4; k++) {
					if ((v >> k) & 1) {
						acc[0] ^= cols[n * 4 + k][0];
						acc[1] ^= cols[n * 4 + k][1];
					}
				}
				spn1_mix_nib[n][v][0] = acc[0];
				spn1_mix_nib[n][v][1] = acc[1];
			}
		}
		spn1_mix_const[0] = load_le64(kSpn1MixConst);
		spn1_mix_const[1] = load_le64(kSpn1MixConst + 8);
	}
};

const Tables &tables()
{
	static const Tables t;
	return t;
}

/* ---- Phase 1: white-box AES over the m2 challenge (fpbridge/wbaes_*.go) ---- */

/* wbaes_fused_core.go wbaesBlockCore, with wbaesFusedI folded into the lookup. */
void wbaes_block_core(const Tables &t, const uint8_t input[16], uint8_t raw[16])
{
	uint8_t state[16];
	std::memcpy(state, input, 16);
	for (int rnd = 0; rnd < 9; rnd++) {
		const uint8_t *order = kTypeIOrder + rnd * 16;
		const uint8_t *mix = kMixingConsts + rnd * 16;
		uint8_t sub[16];
		for (int i = 0; i < 16; i++)
			sub[i] = t.type_i[order[kMixingSigma[i]]][state[i]] ^ mix[i];
		for (int col = 0; col < 4; col++) {
			const uint8_t *p = kShiftRowsCols + col * 4;
			store_le32(state + col * 4, t.type_ii[0][sub[p[0]]] ^ t.type_ii[1][sub[p[1]]] ^
							    t.type_ii[2][sub[p[2]]] ^ t.type_ii[3][sub[p[3]]]);
		}
	}
	const uint8_t *order9 = kTypeIOrder + 9 * 16;
	for (int i = 0; i < 16; i++)
		raw[i] = t.type_i[order9[i]][state[kMixingSigmaInv[i]]];
}

/* wbaes_phase1.go wbaesFullPhase1 */
void wbaes_full_phase1(const Tables &t, const uint8_t payload[128], uint8_t gp[128])
{
	for (int block = 0; block < 8; block++) {
		uint8_t raw[16], out[16];
		wbaes_block_core(t, payload + block * 16, raw);
		for (int i = 0; i < 16; i++)
			out[kGpBufferPerm[i]] = block == 0 ? t.output_dec[i][raw[i]] : (uint8_t)(raw[i] ^ 0x0f);
		for (int i = 0; i < 16; i++)
			gp[block * 16 + i] = block == 0 ? out[i] : (uint8_t)(out[i] ^ payload[(block - 1) * 16 + i]);
	}
}

/* ---- Phase 2: the hash over the bridge digest (fairplayhash) ---- */

/* prologue.go neonBlock, per 32-bit lane, with the constant masks every caller here uses. */
uint32_t neon_lane(uint32_t v)
{
	return (v ^ kBridgeVreg1[0]) + ((v << 1) & kBridgeVreg3[0]) + kBridgeVreg2[0];
}

/* spn1_r8r19.go hiddenFromStaging, and prologue.go ComputeHiddenWords for round 0. */
void hidden_words(const uint8_t src[64], uint32_t hidden[16])
{
	for (int i = 0; i < 16; i++)
		hidden[i] = neon_lane(load_le32(src + i * 4));
}

/* prologue.go shuffleHiddenG2Into */
void shuffle_hidden_g2(uint32_t h[16], uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	const uint32_t picks[8] = {(a ^ 3) & 15,        (b ^ 0xd) & 15, (c ^ 0xb) & 15,        (d ^ 6) & 15,
				   ((a >> 4) ^ 1) & 15, (b >> 4) & 15,  ((c >> 4) ^ 0xe) & 15, ((d >> 4) ^ 4) & 15};
	for (int i = 0; i < 8; i++) {
		uint32_t swap = h[i];
		h[i] = h[picks[i]];
		h[picks[i]] = swap;
	}
}

/* roundc_plain.go RoundC_MD5PlainReference, always deriving G2 from G0. */
void round_c(uint32_t state[4], const uint32_t g0[16])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t g2[16];
	for (int i = 0; i < 64; i++) {
		if (i == 32) {
			std::memcpy(g2, g0, sizeof g2);
			shuffle_hidden_g2(g2, a ^ kOutBiases[28], b ^ kOutBiases[31], c ^ kOutBiases[30],
					  d ^ kOutBiases[29]);
		}
		uint32_t f;
		switch (i >> 4) {
		case 0:
			f = d ^ (b & (c ^ d));
			break;
		case 1:
			f = c ^ (d & (b ^ c));
			break;
		case 2:
			f = b ^ c ^ d;
			break;
		default:
			f = c ^ (b | ~d);
			break;
		}
		uint32_t msg = i < 32 ? g0[kMsgSchedule[i]] : g2[kMsgSchedule[i]];
		uint32_t tmp = rotl32(a + msg + f + kPlainAddConsts[i], 32u - kRorAmounts[i]);
		a = d;
		d = c;
		c = b;
		b = tmp + b;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}

void be_words(const uint32_t words[4], uint8_t out[16])
{
	for (int w = 0; w < 4; w++)
		store_be32(out + w * 4, words[w]);
}

void bswap_words(const uint8_t in[16], uint8_t out[16])
{
	for (int w = 0; w < 4; w++)
		for (int k = 0; k < 4; k++)
			out[w * 4 + k] = in[w * 4 + 3 - k];
}

/* spn1_mixcol_fast.go ApplyMixColumns */
void apply_mix_columns(const Tables &t, const uint8_t in[16], uint8_t out[16])
{
	uint64_t o0 = t.spn1_mix_const[0], o1 = t.spn1_mix_const[1];
	for (int i = 0; i < 16; i++) {
		const uint64_t *lo = t.spn1_mix_nib[i * 2][in[i] & 0xf];
		const uint64_t *hi = t.spn1_mix_nib[i * 2 + 1][in[i] >> 4];
		o0 ^= lo[0] ^ hi[0];
		o1 ^= lo[1] ^ hi[1];
	}
	store_le64(out, o0);
	store_le64(out + 8, o1);
}

/* spn1.go SPN1 and spn1_trailing.go ApplyTrailing */
void spn1(const Tables &t, const uint32_t ro[20][4], uint8_t out[16])
{
	uint8_t state[16];
	std::memcpy(state, kSpn1CoreInput, 16);
	for (int r = 0; r < 9; r++) {
		uint8_t stage[16], mixed[16];
		for (int o = 0; o < 16; o++)
			stage[o] = t.spn1_core[r][o][state[kSpn1ShiftRows[o]]];
		apply_mix_columns(t, stage, mixed);
		for (int w = 0; w < 4; w++)
			store_be32(state + w * 4, load_be32(mixed + w * 4) ^ ro[10 + r][w]);
	}
	for (int p = 0; p < 16; p++)
		out[p] = t.spn1_trail[p][state[kSpn1TrailShiftRows[p]]];
}

/* tail_spn_fast.go TailSPN */
void tail_spn(const Tables &t, const uint8_t input[16], const uint8_t mix_table[144], uint8_t out[16])
{
	uint8_t state[16];
	std::memcpy(state, input, 16);
	for (int round = 0; round < 9; round++) {
		if (round > 0) {
			const uint8_t *type_i = kTailTypeI + round * 16;
			const uint8_t *mix = mix_table + (8 - round) * 16;
			uint8_t next[16];
			for (int pos = 0; pos < 16; pos++)
				next[pos] = t.type_i[type_i[15 - pos]][state[kTailInvShiftRows[pos]]] ^ mix[pos];
			std::memcpy(state, next, 16);
		}
		for (int col = 0; col < 4; col++) {
			uint8_t *s = state + col * 4;
			store_le32(s,
				   t.type_ii[0][s[0]] ^ t.type_ii[1][s[1]] ^ t.type_ii[2][s[2]] ^ t.type_ii[3][s[3]]);
		}
	}
	for (int p = 0; p < 16; p++)
		out[p] = t.tail_final[p][state[kTailInvShiftRows[p]]];
}

/* analytical.go ComputeRoundOutputsAnalytical and ComputeHashAnalytical, via
 * fp_exchange_native.go exchangeFromX9. */
void exchange_from_x9(const Tables &t, const uint8_t x9[64], uint8_t result[20])
{
	static const uint32_t kNormalRoundIv[4] = {0x1d4a4587, 0x92f39fcc, 0x1d87d836, 0xcdc86697};
	uint32_t ro[20][4];
	uint32_t base[16];
	hidden_words(x9, base);
	for (int k = 0; k <= 8; k++) {
		uint32_t h[16];
		std::memcpy(h, base, sizeof h);
		h[5] += (uint32_t)k << 24;
		std::memcpy(ro[10 + k], kNormalRoundIv, sizeof kNormalRoundIv);
		round_c(ro[10 + k], h);
	}
	std::memcpy(ro[9], kNormalRoundIv, sizeof kNormalRoundIv);
	for (int k = 0; k < 8; k++)
		std::memcpy(ro[k], ro[11 + k], sizeof ro[k]);

	uint8_t mix_table[144];
	for (int b = 0; b < 9; b++)
		be_words(ro[10 + b], mix_table + b * 16);

	uint8_t spn1_out[16];
	spn1(t, ro, spn1_out);
	{
		/* spn1_r8r19.go R8Staging */
		uint8_t src[64];
		bswap_words(spn1_out, src);
		std::memcpy(src + 16, kR8Suffix, 48);
		uint32_t g0[16];
		hidden_words(src, g0);
		std::memcpy(ro[8], kRound8InitialState, sizeof ro[8]);
		round_c(ro[8], g0);
	}

	/* spn1_ground.go TailInput */
	uint8_t be8[16], be18[16], tail_in[16], tail_out[16];
	be_words(ro[8], be8);
	be_words(ro[18], be18);
	for (int p = 0; p < 16; p++)
		tail_in[p] = t.ground[p][be8[kGroundPerm[p]]] ^ be18[p];
	tail_spn(t, tail_in, mix_table, tail_out);
	{
		/* spn1_r8r19.go R19Staging */
		uint8_t src[64];
		for (int w = 0; w < 4; w++)
			store_le32(src + w * 4, ro[8][w]);
		bswap_words(tail_out, src + 16);
		std::memcpy(src + 32, kMdPadLen, 32);
		uint32_t g0[16];
		hidden_words(src, g0);
		std::memcpy(ro[19], kRound8InitialState, sizeof ro[19]);
		round_c(ro[19], g0);
	}

	std::memcpy(result, tail_out, 4);
	for (int w = 0; w < 4; w++)
		store_be32(result + 4 + w * 4, ro[19][w]);
}

/* ---- The m3 body and the key wrap (fpsapcore) ---- */

void shift_rows(uint8_t s[16], bool inverse)
{
	uint8_t prev[16];
	std::memcpy(prev, s, 16);
	for (int row = 0; row < 4; row++)
		for (int col = 0; col < 4; col++)
			s[4 * col + row] = prev[4 * ((inverse ? col - row + 4 : col + row) & 3) + row];
}

void xor_key(uint8_t s[16], const uint8_t *key)
{
	for (int i = 0; i < 16; i++)
		s[i] ^= key[i];
}

/* message_encrypt.go encryptMessageBlockMode3 */
void encrypt_block(const Tables &t, uint8_t s[16])
{
	xor_key(s, kMsgRoundKey0);
	for (int round = 0; round <= 9; round++) {
		if (round > 0) {
			for (int c = 0; c < 4; c++) {
				uint8_t *o = s + c * 4;
				uint8_t a = o[0], b = o[1], cc = o[2], d = o[3];
				o[0] = t.mul2[a] ^ t.mul3[b] ^ cc ^ d;
				o[1] = a ^ t.mul2[b] ^ t.mul3[cc] ^ d;
				o[2] = a ^ b ^ t.mul2[cc] ^ t.mul3[d];
				o[3] = t.mul3[a] ^ b ^ cc ^ t.mul2[d];
			}
			xor_key(s, kMsgMiddleKeys + (round - 1) * 16);
		}
		for (int i = 0; i < 16; i++)
			s[i] = kAesSbox[s[i]];
		shift_rows(s, false);
	}
	xor_key(s, kMsgRoundKey10);
}

/* wrap.go decryptMessageBlockMode3 */
void decrypt_block(const Tables &t, uint8_t s[16])
{
	xor_key(s, kMsgRoundKey10);
	for (int round = 9; round > 0; round--) {
		shift_rows(s, true);
		for (int i = 0; i < 16; i++)
			s[i] = t.inverse_sbox[s[i]];
		xor_key(s, kMsgMiddleKeys + (round - 1) * 16);
		for (int c = 0; c < 4; c++) {
			uint8_t *o = s + c * 4;
			uint8_t a = o[0], b = o[1], cc = o[2], d = o[3];
			o[0] = t.mul14[a] ^ t.mul11[b] ^ t.mul13[cc] ^ t.mul9[d];
			o[1] = t.mul9[a] ^ t.mul14[b] ^ t.mul11[cc] ^ t.mul13[d];
			o[2] = t.mul13[a] ^ t.mul9[b] ^ t.mul14[cc] ^ t.mul11[d];
			o[3] = t.mul11[a] ^ t.mul13[b] ^ t.mul9[cc] ^ t.mul14[d];
		}
	}
	shift_rows(s, true);
	for (int i = 0; i < 16; i++)
		s[i] = t.inverse_sbox[s[i]];
	xor_key(s, kMsgRoundKey0);
}

/* message_encrypt.go EncryptMessageBodyMode3: CBC under the mode-3 schedule. */
void encrypt_body(const Tables &t, const uint8_t plain[128], uint8_t out[128])
{
	const uint8_t *chain = kMsgIv;
	for (int block = 0; block < 8; block++) {
		uint8_t *s = out + block * 16;
		for (int i = 0; i < 16; i++)
			s[i] = plain[block * 16 + i] ^ chain[i];
		encrypt_block(t, s);
		chain = s;
	}
}

/* wrap.go DecryptMessageBodyMode3 */
void decrypt_body(const Tables &t, const uint8_t cipher[128], uint8_t out[128])
{
	for (int block = 0; block < 8; block++) {
		uint8_t s[16];
		std::memcpy(s, cipher + block * 16, 16);
		decrypt_block(t, s);
		const uint8_t *chain = block == 0 ? kMsgIv : cipher + (block - 1) * 16;
		for (int i = 0; i < 16; i++)
			out[block * 16 + i] = s[i] ^ chain[i];
	}
}

/* fairplay_md5.go fairplayMD5CompressReference with the KDF mutation. */
void md5_compress_kdf(uint32_t state[4], const uint8_t block[64], uint32_t out[4])
{
	uint32_t m[16];
	for (int i = 0; i < 16; i++)
		m[i] = load_be32(block + i * 4);
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	for (int round = 0; round < 64; round++) {
		uint32_t f;
		int word;
		if (round < 16) {
			f = (b & c) | (~b & d);
			word = round;
		} else if (round < 32) {
			f = (d & b) | (~d & c);
			word = (5 * round + 1) & 15;
		} else if (round < 48) {
			f = b ^ c ^ d;
			word = (3 * round + 5) & 15;
		} else {
			f = c ^ (b | ~d);
			word = (7 * round) & 15;
		}
		uint32_t next = b + rotl32(a + f + kMd5Constant[round] + m[word], kMd5Shift[round]);
		a = d;
		d = c;
		c = b;
		b = next;
		if (round == 31) {
			auto swap = [&m](uint32_t i, uint32_t j) {
				uint32_t v = m[i & 15];
				m[i & 15] = m[j & 15];
				m[j & 15] = v;
			};
			swap(a, b);
			swap(c, d);
			for (int shift = 4; shift <= 12; shift += 4)
				swap(a >> shift, b >> shift);
		}
	}
	out[0] = state[0] + a;
	out[1] = state[1] + b;
	out[2] = state[2] + c;
	out[3] = state[3] + d;
}

/* fp_sap_m3.go parseFPSAPM2 */
bool parse_m2(const uint8_t *m2, size_t size, const char **error)
{
	const char *problem = nullptr;
	if (!m2 || size != kM2Size)
		problem = "m2 has the wrong length";
	else if (std::memcmp(m2, "FPLY", 4) != 0 || m2[4] != 3 || m2[5] != 1 || m2[6] != 2 || m2[7] != 0 ||
		 load_be32(m2 + 8) != 130 || m2[12] != 2)
		problem = "m2 is not a FairPlay challenge";
	else if (m2[13] != 3)
		problem = "receiver selected a FairPlay mode other than 3";
	if (problem && error)
		*error = problem;
	return !problem;
}

} /* namespace */

/* fp_sap_m1.go NewFPSAPM1(FPSAPFullCapabilities) */
void build_m1(uint8_t out[kM1Size])
{
	static const uint8_t kM1[kM1Size] = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 0x02, 0x00, 0x03, 0xbb};
	std::memcpy(out, kM1, kM1Size);
}

/* fp_sap_session.go NewFPSAPSession and ExchangeM3 */
bool build_m3(const uint8_t *m2, size_t m2_size, const uint8_t entropy[kLocalSapEntropy], uint8_t out[kM3Size],
	      const char **error)
{
	if (!parse_m2(m2, m2_size, error))
		return false;
	const Tables &t = tables();
	uint8_t local_sap[128] = {0, 1};
	std::memcpy(local_sap + 2, entropy, kLocalSapEntropy);

	static const uint8_t kHeader[16] = {'F', 'P', 'L', 'Y', 3, 1, 3, 0, 0, 0, 0, 152, 3, 0x8f, 0x1a, 0x9c};
	std::memcpy(out, kHeader, 16);
	encrypt_body(t, local_sap, out + 16);

	uint8_t gp[128], x9[64];
	wbaes_full_phase1(t, m2 + 14, gp);
	fp_bridge_x9_head_for_sap(local_sap, gp, x9);
	std::memcpy(x9 + 20, kBridgeX9Tail, 44);
	exchange_from_x9(t, x9, out + 144);
	return true;
}

/* wrap.go deriveWrappingKey and the MAC key from WrapStreamKey */
bool wrap_keys(const uint8_t *m2, size_t m2_size, const uint8_t *m3, size_t m3_size, uint8_t aes_key[16],
	       uint8_t mac_key[20], const char **error)
{
	if (!m2 || m2_size != kM2Size || std::memcmp(m2, "FPLY", 4) != 0 || m2[6] != 2 || m2[13] != 3 || !m3 ||
	    m3_size != kM3Size || std::memcmp(m3, "FPLY", 4) != 0 || m3[6] != 3 || m3[12] != 3) {
		if (error)
			*error = "m2 or m3 is not a mode-3 FairPlay record";
		return false;
	}
	const Tables &t = tables();
	uint8_t receiver[128], sender[128];
	decrypt_body(t, m2 + 14, receiver);
	decrypt_body(t, m3 + 16, sender);

	uint8_t material[320] = {};
	size_t offset = 0;
	std::memcpy(material + offset, kKdfPrefix, sizeof kKdfPrefix);
	offset += sizeof kKdfPrefix;
	std::memcpy(material + offset, sender, 128);
	offset += 128;
	std::memcpy(material + offset, receiver, 128);
	offset += 128;
	std::memcpy(material + offset, kKdfSuffix, sizeof kKdfSuffix);
	offset += sizeof kKdfSuffix;
	material[offset] = 0x80;
	store_le64(material + sizeof material - 8, (uint64_t)offset * 8);

	uint32_t state[4];
	for (int w = 0; w < 4; w++)
		state[w] = load_le32(kInitialSessionKey + w * 4);
	for (size_t at = 0; at < sizeof material; at += 64) {
		uint32_t modified[4];
		uint8_t hashed[16];
		md5_compress_kdf(state, material + at, modified);
		fp_sap_hash(material + at, hashed);
		for (int w = 0; w < 4; w++)
			state[w] = modified[w] + load_le32(hashed + w * 4);
	}
	be_words(state, aes_key);
	fp_sap_descriptor_for_sap(sender, receiver, mac_key);
	return true;
}

} /* namespace fairplay_sap */
