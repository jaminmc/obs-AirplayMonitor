/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

extern "C" void airplay_random_bytes(void *bytes, size_t size);

namespace airplay {

/* Cryptographically secure random bytes from the operating system. */
inline void random_bytes(void *bytes, size_t size)
{
	airplay_random_bytes(bytes, size);
}

/* 64x64 to 128-bit multiply. MSVC has no 128-bit integer type. */
inline uint64_t mul64(uint64_t a, uint64_t b, uint64_t &hi)
{
#if defined(_MSC_VER) && !defined(__clang__)
	return _umul128(a, b, &hi);
#else
	unsigned __int128 product = (unsigned __int128)a * b;
	hi = (uint64_t)(product >> 64);
	return (uint64_t)product;
#endif
}

/* Returns a + b + carry_in and sets carry_out to 0 or 1. */
inline uint64_t add_carry(uint64_t a, uint64_t b, uint64_t carry_in, uint64_t &carry_out)
{
	uint64_t sum = a + b;
	uint64_t carry = sum < a;
	uint64_t total = sum + carry_in;
	carry_out = carry | (total < sum);
	return total;
}

} /* namespace airplay */
