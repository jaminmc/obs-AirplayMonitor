/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/features.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace airplay {

struct DiscoveredReceiver : ReceiverInfo {
	RejectReason reason = RejectReason::NoScreen;
};

/* One resource record from an mDNS answer. Names are lowercase dotted text ending in a dot. */
struct MdnsRecord {
	std::string name;
	uint16_t type = 0;
	/* PTR and SRV target. */
	std::string target;
	/* SRV port. */
	uint16_t port = 0;
	/* TXT strings, length-prefixed as on the wire. */
	std::vector<uint8_t> txt;
	/* A or AAAA as numeric text. */
	std::string address;
	/* The PTR target's first label with its original case, which is the receiver's display name. */
	std::string instance;
};

std::vector<uint8_t> mdns_query(uint16_t id, const std::vector<std::pair<std::string, uint16_t>> &questions);
std::vector<MdnsRecord> mdns_parse(const uint8_t *packet, size_t size);

class Discovery {
public:
	using Callback = std::function<void(std::vector<DiscoveredReceiver>)>;

	Discovery();
	~Discovery();

	void start(Callback callback);
	void stop();

private:
	struct Impl;
	Impl *impl_;
};

} /* namespace airplay */
