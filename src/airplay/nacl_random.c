/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Operating-system randomness for the plugin and for TweetNaCl, which calls
 * randombytes and does not define it.
 */
#include <stddef.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
/* arc4random_buf is in stdlib.h. */
#else
#include <errno.h>
#include <sys/random.h>
#endif

void airplay_random_bytes(void *bytes, size_t size)
{
	if (!bytes || !size)
		return;
#if defined(_WIN32)
	if (BCryptGenRandom(NULL, (PUCHAR)bytes, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		abort();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	arc4random_buf(bytes, size);
#else
	unsigned char *out = (unsigned char *)bytes;
	while (size) {
		ssize_t got = getrandom(out, size, 0);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			abort();
		}
		out += got;
		size -= (size_t)got;
	}
#endif
}

void randombytes(unsigned char *bytes, unsigned long long length)
{
	airplay_random_bytes(bytes, (size_t)length);
}
