/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/fairplay.hpp"

#include "airplay/crypto.hpp"

#ifndef AIRPLAY_FAIRPLAY_STUB
#include "airplay/portable.hpp"
#include "cpp/fairplay_sender.hpp"
#endif

#include <cstdint>
#include <cstring>

namespace airplay {
namespace {

#ifdef AIRPLAY_FAIRPLAY_STUB
const char *kUnavailable = "FairPlay SAP is not available in this build.";
#else
const char *kFailed = "FairPlay setup failed.";
#endif

#ifdef AIRPLAY_FAIRPLAY_STUB
FairPlayHooks g_hooks{};
bool g_have_hooks = false;
#endif

} /* namespace */

FairPlayResult fairplay_exchange(const std::vector<uint8_t> &challenge)
{
	(void)challenge;
	FairPlayResult result;
	result.ok = false;
#ifdef AIRPLAY_FAIRPLAY_STUB
	result.error = kUnavailable;
#else
	result.error = kFailed;
#endif
	return result;
}

#ifdef AIRPLAY_FAIRPLAY_STUB
void fairplay_install_hooks(const FairPlayHooks *hooks)
{
	if (!hooks) {
		g_have_hooks = false;
		g_hooks = {};
		return;
	}
	g_hooks = *hooks;
	g_have_hooks = true;
}
#endif

void fairplay_ekey(const uint8_t aes_key[16], const uint8_t mac_key[20], const uint8_t raw[16], const uint8_t mask[16],
		   std::vector<uint8_t> &ekey)
{
	static const uint8_t kHeader[16] = {'F', 'P', 'L', 'Y', 1, 2, 1, 0, 0, 0, 0, 0x3c, 0, 0, 0, 0};
	ekey.assign(72, 0);
	std::memcpy(ekey.data(), kHeader, sizeof kHeader);
	std::memcpy(ekey.data() + 16, mask, 16);
	ekey[35] = 16;
	uint8_t masked[16];
	for (int i = 0; i < 16; i++)
		masked[i] = raw[i] ^ mask[i];
	aes128_encrypt_block(aes_key, masked, ekey.data() + 56);
	uint8_t signed_data[36 + 16];
	std::memcpy(signed_data, ekey.data(), 36);
	std::memcpy(signed_data + 36, raw, 16);
	hmac_sha1(mac_key, 20, signed_data, sizeof signed_data, ekey.data() + 36);
	std::memset(masked, 0, sizeof masked);
	std::memset(signed_data, 0, sizeof signed_data);
}

FairPlaySap::FairPlaySap() = default;

FairPlaySap::~FairPlaySap() = default;

bool FairPlaySap::m1(std::vector<uint8_t> &out, std::string &error)
{
#ifdef AIRPLAY_FAIRPLAY_STUB
	if (!g_have_hooks || !g_hooks.m1) {
		error = kUnavailable;
		return false;
	}
	return g_hooks.m1(g_hooks.ctx, out, error);
#else
	out.assign(fairplay_sap::kM1Size, 0);
	fairplay_sap::build_m1(out.data());
	error.clear();
	return true;
#endif
}

bool FairPlaySap::m3(const std::vector<uint8_t> &m2, std::vector<uint8_t> &out, std::string &error)
{
#ifdef AIRPLAY_FAIRPLAY_STUB
	if (!g_have_hooks || !g_hooks.m3) {
		error = kUnavailable;
		return false;
	}
	return g_hooks.m3(g_hooks.ctx, m2, out, error);
#else
	uint8_t entropy[fairplay_sap::kLocalSapEntropy];
	const char *reason = nullptr;
	out.assign(fairplay_sap::kM3Size, 0);
	random_bytes(entropy, sizeof entropy);
	bool ok = fairplay_sap::build_m3(m2.data(), m2.size(), entropy, out.data(), &reason);
	std::memset(entropy, 0, sizeof entropy);
	if (!ok) {
		error = reason ? std::string(kFailed) + " " + reason : kFailed;
		out.clear();
		return false;
	}
	error.clear();
	return true;
#endif
}

bool FairPlaySap::wrap(const std::vector<uint8_t> &m2, const std::vector<uint8_t> &m3, const uint8_t raw[16],
		       std::vector<uint8_t> &ekey, std::string &error)
{
#ifdef AIRPLAY_FAIRPLAY_STUB
	if (!g_have_hooks || !g_hooks.wrap) {
		error = kUnavailable;
		return false;
	}
	return g_hooks.wrap(g_hooks.ctx, m2, m3, raw, ekey, error);
#else
	uint8_t aes_key[16], mac_key[20], mask[16];
	const char *reason = nullptr;
	random_bytes(mask, sizeof mask);
	bool ok = raw && fairplay_sap::wrap_keys(m2.data(), m2.size(), m3.data(), m3.size(), aes_key, mac_key, &reason);
	if (ok)
		fairplay_ekey(aes_key, mac_key, raw, mask, ekey);
	std::memset(aes_key, 0, sizeof aes_key);
	std::memset(mac_key, 0, sizeof mac_key);
	if (!ok) {
		error = reason ? std::string(kFailed) + " " + reason : kFailed;
		ekey.clear();
		return false;
	}
	error.clear();
	return true;
#endif
}

} /* namespace airplay */
