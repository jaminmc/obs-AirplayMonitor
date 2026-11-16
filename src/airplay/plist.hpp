/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace airplay {

struct Plist {
	enum class Type { Null, Bool, Int, Real, String, Data, Array, Dict };

	Type type = Type::Null;
	bool b = false;
	int64_t i = 0;
	double r = 0;
	std::string s;
	std::vector<uint8_t> data;
	std::vector<Plist> array;
	std::map<std::string, Plist> dict;

	static Plist boolean(bool value);
	static Plist integer(int64_t value);
	static Plist real(double value);
	static Plist string(std::string value);
	static Plist blob(std::vector<uint8_t> value);
	static Plist arr(std::vector<Plist> value);
	static Plist dictionary(std::map<std::string, Plist> value);

	const Plist *find(const std::string &key) const;
	int64_t get_int(const std::string &key, int64_t fallback = 0) const;
	double get_real(const std::string &key, double fallback = 0) const;
	std::string get_string(const std::string &key) const;
	bool get_bool(const std::string &key, bool fallback = false) const;
};

bool encode_bplist(const Plist &plist, std::vector<uint8_t> &out, std::string &error);
bool decode_plist(const uint8_t *bytes, size_t len, Plist &out, std::string &error);

} /* namespace airplay */
