/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/handshake.hpp"

#include <cstdint>
#include <string>

namespace airplay {

class TcpTransport : public ByteTransport {
public:
	TcpTransport() = default;
	~TcpTransport() override;
	TcpTransport(const TcpTransport &) = delete;
	TcpTransport &operator=(const TcpTransport &) = delete;

	bool connect_to(const std::string &address, uint16_t port, int family, int timeout_ms, std::string &error);
	/* Bound the kernel's unsent video bytes so a slow receiver drops frames here instead of queueing delay. */
	void prefer_low_latency();
	void close();
	bool is_open() const { return fd_ != -1; }
	/* The local end of the connection as numeric text, empty if unknown. */
	std::string local_address() const;
	/* Copies waiting bytes without consuming them or blocking. Returns the count, or -1. */
	int peek(uint8_t *data, size_t capacity);
	bool write_all(const uint8_t *data, size_t size) override;
	int read_some(uint8_t *data, size_t capacity, int timeout_ms) override;

private:
	intptr_t fd_ = -1;
};

class UdpClock {
public:
	~UdpClock();
	bool bind_any(int family, std::string &error);
	uint16_t port() const { return port_; }
	/* Sends one datagram from this socket. A pending ICMP error is discarded first so it cannot swallow the send. */
	bool send_to(const std::string &address, uint16_t port, int family, const uint8_t *data, size_t size);
	/* Reads one datagram if one is waiting and writes the NTP reply. */
	void pump(uint64_t now_ntp);
	/* Sends three AirPlay timing requests to a receiver port. */
	void probe_receiver(const std::string &address, uint16_t port, uint64_t now_ntp);
	int replies() const { return replies_; }
	void close();

private:
	intptr_t fd_ = -1;
	uint16_t port_ = 0;
	int replies_ = 0;
};

} /* namespace airplay */
