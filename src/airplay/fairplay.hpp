/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

struct FairPlayResult {
	bool ok = false;
	std::string error;
	std::vector<uint8_t> response;
};

/* Kept so a caller cannot bypass the FairPlay module. The handshake uses
 * FairPlaySap instead of this entry point. */
FairPlayResult fairplay_exchange(const std::vector<uint8_t> &challenge);

#ifdef AIRPLAY_FAIRPLAY_STUB
struct FairPlayHooks {
	bool (*m1)(void *ctx, std::vector<uint8_t> &out, std::string &error);
	bool (*m3)(void *ctx, const std::vector<uint8_t> &m2, std::vector<uint8_t> &out, std::string &error);
	bool (*wrap)(void *ctx, const std::vector<uint8_t> &m2, const std::vector<uint8_t> &m3, const uint8_t raw[16],
		     std::vector<uint8_t> &ekey, std::string &error);
	void *ctx;
};

void fairplay_install_hooks(const FairPlayHooks *hooks);
#endif

/* The 72-byte ekey: the stream key masked, encrypted under the session's wrap key, and signed. */
void fairplay_ekey(const uint8_t aes_key[16], const uint8_t mac_key[20], const uint8_t raw[16], const uint8_t mask[16],
		   std::vector<uint8_t> &ekey);

/* One FairPlay SAP session: m1, the receiver's m2, m3, then the 72-byte ekey. */
class FairPlaySap {
public:
	FairPlaySap();
	~FairPlaySap();
	FairPlaySap(const FairPlaySap &) = delete;
	FairPlaySap &operator=(const FairPlaySap &) = delete;

	bool m1(std::vector<uint8_t> &out, std::string &error);
	bool m3(const std::vector<uint8_t> &m2, std::vector<uint8_t> &out, std::string &error);
	bool wrap(const std::vector<uint8_t> &m2, const std::vector<uint8_t> &m3, const uint8_t raw[16],
		  std::vector<uint8_t> &ekey, std::string &error);
};

} /* namespace airplay */
