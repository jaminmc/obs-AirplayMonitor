/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

namespace airplay {

/* Loads a shared library by file name. On Windows the plugin's own folder is searched first. */
void *open_library(const char *name);
void *library_symbol(void *library, const char *name);

template<typename T> bool library_function(void *library, const char *name, T &out)
{
	out = reinterpret_cast<T>(library_symbol(library, name));
	return out != nullptr;
}

} /* namespace airplay */
