/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/socket.hpp"

#include "airplay/net.hpp"
#include "airplay/ntp.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace airplay {
namespace {

bool fill_address(const std::string &address, uint16_t port, int family, sockaddr_storage &storage,
		  socklen_t &length)
{
	std::memset(&storage, 0, sizeof storage);
	if (family == AF_INET6) {
		auto *addr = reinterpret_cast<sockaddr_in6 *>(&storage);
		addr->sin6_family = AF_INET6;
		addr->sin6_port = htons(port);
		length = sizeof(sockaddr_in6);
		return inet_pton(AF_INET6, address.c_str(), &addr->sin6_addr) == 1;
	}
	auto *addr = reinterpret_cast<sockaddr_in *>(&storage);
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	length = sizeof(sockaddr_in);
	return inet_pton(AF_INET, address.c_str(), &addr->sin_addr) == 1;
}

} /* namespace */

TcpTransport::~TcpTransport()
{
	close();
}

void TcpTransport::close()
{
	if (fd_ != -1) {
		net::close(net::from_handle(fd_));
		fd_ = -1;
	}
}

bool TcpTransport::connect_to(const std::string &address, uint16_t port, int family, int timeout_ms, std::string &error)
{
	close();
	sockaddr_storage storage;
	socklen_t length = 0;
	if (!fill_address(address, port, family, storage, length)) {
		error = family == AF_INET6 ? "Bad IPv6 address" : "Bad IPv4 address";
		return false;
	}
	net::Socket fd = net::open(family, SOCK_STREAM);
	if (!net::valid(fd)) {
		error = "Could not open a socket";
		return false;
	}
	net::set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
	net::no_sigpipe(fd);
	/* A receiver that stops reading must not leave Disconnect blocked in send(). */
	net::set_send_timeout(fd, 5);
	net::set_nonblocking(fd, true);
	int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&storage), length);
	if (rc != 0 && !net::connect_pending(net::last_error())) {
		net::close(fd);
		error = "Could not connect to the receiver";
		return false;
	}
	if (rc != 0 && !net::wait(fd, true, timeout_ms)) {
		net::close(fd);
		error = "Timed out connecting to the receiver";
		return false;
	}
	int so_error = 0;
	socklen_t so_len = sizeof so_error;
	getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &so_len);
	if (so_error != 0) {
		net::close(fd);
		error = "The receiver refused the connection";
		return false;
	}
	net::set_nonblocking(fd, false);
	fd_ = net::to_handle(fd);
	return true;
}

void TcpTransport::prefer_low_latency()
{
#if defined(TCP_NOTSENT_LOWAT)
	if (fd_ != -1)
		net::set_int_option(net::from_handle(fd_), IPPROTO_TCP, TCP_NOTSENT_LOWAT, 32 * 1024);
#endif
}

std::string TcpTransport::local_address() const
{
	if (fd_ == -1)
		return {};
	sockaddr_storage local;
	socklen_t length = sizeof local;
	if (getsockname(net::from_handle(fd_), reinterpret_cast<sockaddr *>(&local), &length) != 0)
		return {};
	char text[INET6_ADDRSTRLEN] = {};
	if (local.ss_family == AF_INET)
		inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&local)->sin_addr, text, sizeof text);
	else if (local.ss_family == AF_INET6)
		inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&local)->sin6_addr, text, sizeof text);
	return text;
}

int TcpTransport::peek(uint8_t *data, size_t capacity)
{
	if (fd_ == -1)
		return -1;
	net::Socket fd = net::from_handle(fd_);
	if (!net::wait(fd, false, 0))
		return 0;
	return (int)net::recv(fd, data, capacity, MSG_PEEK);
}

bool TcpTransport::write_all(const uint8_t *data, size_t size)
{
	net::Socket fd = net::from_handle(fd_);
	size_t sent = 0;
	while (sent < size) {
		net::ssize count = net::send(fd, data + sent, size - sent);
		if (count < 0) {
			if (net::interrupted(net::last_error()))
				continue;
			return false;
		}
		if (count == 0)
			return false;
		sent += (size_t)count;
	}
	return true;
}

int TcpTransport::read_some(uint8_t *data, size_t capacity, int timeout_ms)
{
	if (fd_ == -1)
		return -1;
	net::Socket fd = net::from_handle(fd_);
	if (!net::wait(fd, false, timeout_ms))
		return 0;
	net::ssize count = net::recv(fd, data, capacity);
	if (count < 0) {
		int error = net::last_error();
		if (net::interrupted(error) || net::would_block(error))
			return 0;
		return -1;
	}
	return (int)count;
}

UdpClock::~UdpClock()
{
	close();
}

void UdpClock::close()
{
	if (fd_ != -1) {
		net::close(net::from_handle(fd_));
		fd_ = -1;
	}
	port_ = 0;
}

bool UdpClock::bind_any(int family, std::string &error)
{
	close();
	if (family != AF_INET6)
		family = AF_INET;
	net::Socket fd = net::open(family, SOCK_DGRAM);
	if (!net::valid(fd)) {
		error = "Could not open the timing socket";
		return false;
	}
	sockaddr_storage storage;
	std::memset(&storage, 0, sizeof storage);
	socklen_t length = 0;
	if (family == AF_INET6) {
		net::set_int_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1);
		auto *addr = reinterpret_cast<sockaddr_in6 *>(&storage);
		addr->sin6_family = AF_INET6;
		addr->sin6_addr = in6addr_any;
		length = sizeof(sockaddr_in6);
	} else {
		auto *addr = reinterpret_cast<sockaddr_in *>(&storage);
		addr->sin_family = AF_INET;
		addr->sin_addr.s_addr = htonl(INADDR_ANY);
		length = sizeof(sockaddr_in);
	}
	if (bind(fd, reinterpret_cast<sockaddr *>(&storage), length) != 0) {
		net::close(fd);
		error = "Could not bind the timing socket";
		return false;
	}
	sockaddr_storage bound;
	socklen_t bound_length = sizeof bound;
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &bound_length) != 0) {
		net::close(fd);
		error = "Could not read the timing port";
		return false;
	}
	port_ = family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port)
				   : ntohs(reinterpret_cast<sockaddr_in *>(&bound)->sin_port);
	net::no_sigpipe(fd);
	net::set_nonblocking(fd, true);
	fd_ = net::to_handle(fd);
	return true;
}

void UdpClock::pump(uint64_t now_ntp)
{
	if (fd_ == -1)
		return;
	net::Socket fd = net::from_handle(fd_);
	for (;;) {
		uint8_t packet[128];
		sockaddr_storage from;
		socklen_t from_len = sizeof from;
		net::ssize count = net::recv_from(fd, packet, sizeof packet, reinterpret_cast<sockaddr *>(&from), &from_len);
		if (count <= 0)
			return;
		std::vector<uint8_t> reply = ntp_reply(packet, (size_t)count, now_ntp);
		if (reply.empty())
			reply = airplay_timing_reply(packet, (size_t)count, now_ntp);
		if (reply.empty()) {
			replies_++;
			continue;
		}
		if (net::send_to(fd, reply.data(), reply.size(), reinterpret_cast<sockaddr *>(&from), from_len) > 0)
			replies_++;
	}
}

bool UdpClock::send_to(const std::string &address, uint16_t port, int family, const uint8_t *data, size_t size)
{
	if (fd_ == -1 || !data || size == 0 || port == 0)
		return false;
	sockaddr_storage storage;
	socklen_t length = 0;
	if (!fill_address(address, port, family, storage, length))
		return false;
	net::Socket fd = net::from_handle(fd_);
	for (int attempt = 0; attempt < 2; attempt++) {
		/* An unreachable port queues one error. The next send consumes it and does not transmit. */
		int pending = 0;
		socklen_t pending_len = sizeof pending;
		getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&pending), &pending_len);
		if (net::send_to(fd, data, size, reinterpret_cast<sockaddr *>(&storage), length) == (net::ssize)size)
			return true;
	}
	return false;
}

void UdpClock::probe_receiver(const std::string &address, uint16_t port, uint64_t now_ntp)
{
	if (fd_ == -1 || port == 0)
		return;
	sockaddr_storage dest;
	socklen_t length = 0;
	if (!fill_address(address, port, AF_INET, dest, length))
		return;
	constexpr uint64_t kEpoch1900 = 2208988800ull;
	uint64_t stamped = now_ntp + (kEpoch1900 << 32);
	for (uint16_t sequence = 1; sequence <= 3; sequence++) {
		uint8_t request[32] = {};
		request[0] = 0x80;
		request[1] = 0xd2;
		request[2] = (uint8_t)(sequence >> 8);
		request[3] = (uint8_t)sequence;
		store_ntp_be(request + 24, stamped);
		net::send_to(net::from_handle(fd_), request, sizeof request, reinterpret_cast<sockaddr *>(&dest), length);
		if (sequence < 3)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

} /* namespace airplay */
