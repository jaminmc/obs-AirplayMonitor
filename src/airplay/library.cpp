/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/library.hpp"

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace airplay {

#ifdef _WIN32
namespace {

std::wstring plugin_directory()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&plugin_directory), &self))
		return {};
	wchar_t path[MAX_PATH];
	DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return {};
	std::wstring directory(path, length);
	directory.erase(directory.find_last_of(L"\\/") + 1);
	return directory;
}

} /* namespace */
#endif

void *open_library(const char *name)
{
#ifdef _WIN32
	std::wstring directory = plugin_directory();
	if (!directory.empty()) {
		std::wstring full = directory + std::wstring(name, name + std::strlen(name));
		/* Lets the DLLs this one depends on load from the same folder. */
		if (HMODULE module = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
			return module;
	}
	return LoadLibraryA(name);
#else
	return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *library_symbol(void *library, const char *name)
{
	if (!library)
		return nullptr;
#ifdef _WIN32
	return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
	return dlsym(library, name);
#endif
}

} /* namespace airplay */
