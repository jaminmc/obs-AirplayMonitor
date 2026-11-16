/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The socket differences between POSIX and Winsock, for the files that open sockets.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace airplay::net {

#if defined(_WIN32)
using Socket = SOCKET;
using ssize = int;
#else
using Socket = int;
using ssize = ssize_t;
#endif

/* Handles are stored as intptr_t so headers need no socket includes. -1 is no socket on both. */
inline Socket from_handle(intptr_t handle)
{
	return (Socket)handle;
}

inline intptr_t to_handle(Socket socket)
{
	return (intptr_t)socket;
}

inline bool valid(Socket socket)
{
#if defined(_WIN32)
	return socket != INVALID_SOCKET;
#else
	return socket >= 0;
#endif
}

/* Winsock needs WSAStartup before the first socket. Safe to call from any thread, any number of times. */
inline void startup()
{
#if defined(_WIN32)
	static const bool started = []() {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)started;
#endif
}

inline Socket open(int family, int type)
{
	startup();
	return ::socket(family, type, 0);
}

inline void close(Socket socket)
{
#if defined(_WIN32)
	closesocket(socket);
#else
	::close(socket);
#endif
}

inline void set_nonblocking(Socket socket, bool on)
{
#if defined(_WIN32)
	u_long mode = on ? 1 : 0;
	ioctlsocket(socket, FIONBIO, &mode);
#else
	int flags = fcntl(socket, F_GETFL, 0);
	fcntl(socket, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

inline int set_int_option(Socket socket, int level, int name, int value)
{
	return setsockopt(socket, level, name, reinterpret_cast<const char *>(&value), sizeof value);
}

inline void set_send_timeout(Socket socket, int seconds)
{
#if defined(_WIN32)
	DWORD ms = (DWORD)seconds * 1000;
	setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
#else
	timeval timeout{};
	timeout.tv_sec = seconds;
	setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#endif
}

/* Writes to a closed peer must fail with an error, not raise SIGPIPE. */
inline void no_sigpipe(Socket socket)
{
#if defined(SO_NOSIGPIPE)
	set_int_option(socket, SOL_SOCKET, SO_NOSIGPIPE, 1);
#else
	(void)socket;
#endif
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

inline ssize send(Socket socket, const uint8_t *data, size_t size)
{
	return ::send(socket, reinterpret_cast<const char *>(data), (int)size, kSendFlags);
}

inline ssize recv(Socket socket, uint8_t *data, size_t size, int flags = 0)
{
	return ::recv(socket, reinterpret_cast<char *>(data), (int)size, flags);
}

inline ssize send_to(Socket socket, const uint8_t *data, size_t size, const sockaddr *to, socklen_t length)
{
	return ::sendto(socket, reinterpret_cast<const char *>(data), (int)size, kSendFlags, to, length);
}

inline ssize recv_from(Socket socket, uint8_t *data, size_t size, sockaddr *from, socklen_t *length)
{
	return ::recvfrom(socket, reinterpret_cast<char *>(data), (int)size, 0, from, length);
}

inline int last_error()
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

inline bool interrupted(int error)
{
#if defined(_WIN32)
	return error == WSAEINTR;
#else
	return error == EINTR;
#endif
}

inline bool would_block(int error)
{
#if defined(_WIN32)
	return error == WSAEWOULDBLOCK;
#else
	return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

inline bool connect_pending(int error)
{
#if defined(_WIN32)
	return error == WSAEWOULDBLOCK;
#else
	return error == EINPROGRESS;
#endif
}

/* Waits until the socket is readable, or writable when `write` is set. */
inline bool wait(Socket socket, bool write, int timeout_ms)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(socket, &set);
	timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int rc = select((int)socket + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
	return rc > 0;
}

} /* namespace airplay::net */
