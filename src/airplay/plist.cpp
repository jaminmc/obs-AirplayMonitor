/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/plist.hpp"

#include <cstdlib>
#include <cstring>

namespace airplay {
namespace {

constexpr int kMaxDepth = 32;

void put_be(std::vector<uint8_t> &out, uint64_t value, int bytes)
{
	for (int i = bytes - 1; i >= 0; i--)
		out.push_back((uint8_t)(value >> (8 * i)));
}

uint64_t get_be(const uint8_t *p, size_t bytes)
{
	uint64_t value = 0;
	for (size_t i = 0; i < bytes; i++)
		value = (value << 8) | p[i];
	return value;
}

int width_for(uint64_t value)
{
	if (value <= 0xff)
		return 1;
	if (value <= 0xffff)
		return 2;
	if (value <= 0xffffffffull)
		return 4;
	return 8;
}

void append_utf8(std::string &out, uint32_t cp)
{
	if (cp < 0x80) {
		out.push_back((char)cp);
	} else if (cp < 0x800) {
		out.push_back((char)(0xc0 | (cp >> 6)));
		out.push_back((char)(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		out.push_back((char)(0xe0 | (cp >> 12)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back((char)(0x80 | (cp & 0x3f)));
	} else {
		out.push_back((char)(0xf0 | (cp >> 18)));
		out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
		out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back((char)(0x80 | (cp & 0x3f)));
	}
}

/* UTF-8 to UTF-16 code units. Invalid bytes become U+FFFD. */
std::vector<uint16_t> utf16_of(const std::string &text)
{
	std::vector<uint16_t> units;
	for (size_t i = 0; i < text.size();) {
		uint8_t c = (uint8_t)text[i];
		uint32_t cp = 0xfffd;
		int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (extra < 0 || i + (size_t)extra >= text.size()) {
			i++;
		} else {
			cp = extra == 0 ? c : (uint32_t)(c & (0x3f >> extra));
			for (int k = 1; k <= extra; k++)
				cp = (cp << 6) | ((uint8_t)text[i + (size_t)k] & 0x3f);
			i += (size_t)extra + 1;
		}
		if (cp >= 0x10000) {
			cp -= 0x10000;
			units.push_back((uint16_t)(0xd800 | (cp >> 10)));
			units.push_back((uint16_t)(0xdc00 | (cp & 0x3ff)));
		} else {
			units.push_back((uint16_t)cp);
		}
	}
	return units;
}

class BplistWriter {
public:
	bool write(const Plist &root, std::vector<uint8_t> &out)
	{
		count(root);
		ref_size_ = width_for(objects_ ? objects_ - 1 : 0) > 1 ? 2 : 1;
		if (objects_ > 0xffff)
			ref_size_ = 4;
		body_ = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
		next_ = 0;
		offsets_.assign(objects_, 0);
		emit(root);
		uint64_t table = body_.size();
		int offset_size = width_for(table);
		for (uint64_t offset : offsets_)
			put_be(body_, offset, offset_size);
		body_.insert(body_.end(), 6, 0);
		body_.push_back((uint8_t)offset_size);
		body_.push_back((uint8_t)ref_size_);
		put_be(body_, objects_, 8);
		put_be(body_, 0, 8);
		put_be(body_, table, 8);
		out = std::move(body_);
		return true;
	}

private:
	void count(const Plist &value)
	{
		objects_++;
		if (value.type == Plist::Type::Array) {
			for (const Plist &child : value.array)
				count(child);
		} else if (value.type == Plist::Type::Dict) {
			for (const auto &entry : value.dict) {
				objects_++;
				count(entry.second);
			}
		}
	}

	void marker(uint8_t type, size_t length)
	{
		if (length < 15) {
			body_.push_back((uint8_t)(type | length));
			return;
		}
		body_.push_back((uint8_t)(type | 0x0f));
		int width = width_for(length);
		body_.push_back((uint8_t)(0x10 | (width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3)));
		put_be(body_, length, width);
	}

	void write_string(const std::string &text)
	{
		bool ascii = true;
		for (char c : text)
			ascii = ascii && (uint8_t)c < 0x80;
		if (ascii) {
			marker(0x50, text.size());
			body_.insert(body_.end(), text.begin(), text.end());
			return;
		}
		std::vector<uint16_t> units = utf16_of(text);
		marker(0x60, units.size());
		for (uint16_t unit : units)
			put_be(body_, unit, 2);
	}

	uint64_t emit(const Plist &value)
	{
		uint64_t index = next_++;
		offsets_[index] = body_.size();
		switch (value.type) {
		case Plist::Type::Null:
			body_.push_back(0x00);
			break;
		case Plist::Type::Bool:
			body_.push_back(value.b ? 0x09 : 0x08);
			break;
		case Plist::Type::Int:
			if (value.i < 0) {
				body_.push_back(0x13);
				put_be(body_, (uint64_t)value.i, 8);
			} else {
				int width = width_for((uint64_t)value.i);
				body_.push_back((uint8_t)(0x10 | (width == 1   ? 0
								  : width == 2 ? 1
								  : width == 4 ? 2
									       : 3)));
				put_be(body_, (uint64_t)value.i, width);
			}
			break;
		case Plist::Type::Real: {
			uint64_t bits = 0;
			std::memcpy(&bits, &value.r, sizeof bits);
			body_.push_back(0x23);
			put_be(body_, bits, 8);
			break;
		}
		case Plist::Type::String:
			write_string(value.s);
			break;
		case Plist::Type::Data:
			marker(0x40, value.data.size());
			body_.insert(body_.end(), value.data.begin(), value.data.end());
			break;
		case Plist::Type::Array: {
			marker(0xa0, value.array.size());
			size_t refs = body_.size();
			body_.insert(body_.end(), value.array.size() * (size_t)ref_size_, 0);
			for (size_t i = 0; i < value.array.size(); i++)
				patch(refs + i * (size_t)ref_size_, emit(value.array[i]));
			break;
		}
		case Plist::Type::Dict: {
			size_t n = value.dict.size();
			marker(0xd0, n);
			size_t refs = body_.size();
			body_.insert(body_.end(), 2 * n * (size_t)ref_size_, 0);
			size_t i = 0;
			for (const auto &entry : value.dict) {
				patch(refs + i * (size_t)ref_size_, emit(Plist::string(entry.first)));
				patch(refs + (n + i) * (size_t)ref_size_, emit(entry.second));
				i++;
			}
			break;
		}
		}
		return index;
	}

	void patch(size_t at, uint64_t ref)
	{
		for (int i = 0; i < ref_size_; i++)
			body_[at + (size_t)i] = (uint8_t)(ref >> (8 * (ref_size_ - 1 - i)));
	}

	std::vector<uint8_t> body_;
	std::vector<uint64_t> offsets_;
	uint64_t objects_ = 0;
	uint64_t next_ = 0;
	int ref_size_ = 1;
};

class BplistReader {
public:
	BplistReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

	bool read(Plist &out)
	{
		if (size_ < 40 || std::memcmp(data_, "bplist0", 7) != 0)
			return false;
		const uint8_t *trailer = data_ + size_ - 32;
		offset_size_ = trailer[6];
		ref_size_ = trailer[7];
		objects_ = get_be(trailer + 8, 8);
		uint64_t top = get_be(trailer + 16, 8);
		table_ = get_be(trailer + 24, 8);
		if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8 || objects_ == 0 ||
		    top >= objects_ || table_ >= size_ || objects_ > (size_ - table_) / offset_size_)
			return false;
		return object(top, out, 0);
	}

private:
	bool offset_of(uint64_t ref, uint64_t &offset) const
	{
		if (ref >= objects_)
			return false;
		offset = get_be(data_ + table_ + ref * offset_size_, offset_size_);
		return offset >= 8 && offset < table_;
	}

	bool length_at(uint64_t &pos, uint8_t low, uint64_t &length) const
	{
		if (low != 0x0f) {
			length = low;
			return true;
		}
		if (pos >= table_ || (data_[pos] & 0xf0) != 0x10)
			return false;
		size_t width = (size_t)1 << (data_[pos] & 0x0f);
		if (width > 8 || pos + 1 + width > table_)
			return false;
		length = get_be(data_ + pos + 1, width);
		pos += 1 + width;
		return true;
	}

	bool object(uint64_t ref, Plist &out, int depth)
	{
		uint64_t pos = 0;
		if (depth > kMaxDepth || !offset_of(ref, pos))
			return false;
		uint8_t marker = data_[pos++];
		uint8_t high = marker >> 4;
		uint8_t low = marker & 0x0f;
		switch (high) {
		case 0x0:
			if (marker == 0x08 || marker == 0x09)
				out = Plist::boolean(marker == 0x09);
			else
				out = {};
			return true;
		case 0x1: {
			size_t width = (size_t)1 << low;
			if (width > 16 || pos + width > table_)
				return false;
			/* 16-byte integers hold a 64-bit value in the low half. */
			size_t skip = width > 8 ? width - 8 : 0;
			out = Plist::integer((int64_t)get_be(data_ + pos + skip, width - skip));
			return true;
		}
		case 0x2:
		case 0x3: {
			size_t width = high == 0x3 ? 8 : (size_t)1 << low;
			if ((width != 4 && width != 8) || pos + width > table_)
				return false;
			uint64_t bits = get_be(data_ + pos, width);
			double value = 0;
			if (width == 4) {
				uint32_t narrow = (uint32_t)bits;
				float f = 0;
				std::memcpy(&f, &narrow, sizeof f);
				value = f;
			} else {
				std::memcpy(&value, &bits, sizeof value);
			}
			out = Plist::real(value);
			return true;
		}
		case 0x4:
		case 0x5:
		case 0x6: {
			uint64_t length = 0;
			if (!length_at(pos, low, length))
				return false;
			uint64_t bytes = high == 0x6 ? length * 2 : length;
			if (bytes > table_ - pos)
				return false;
			const uint8_t *p = data_ + pos;
			if (high == 0x4) {
				out = Plist::blob(std::vector<uint8_t>(p, p + bytes));
			} else if (high == 0x5) {
				out = Plist::string(std::string(reinterpret_cast<const char *>(p), (size_t)bytes));
			} else {
				std::string text;
				for (uint64_t i = 0; i < length; i++) {
					uint32_t unit = (uint32_t)get_be(p + i * 2, 2);
					if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < length) {
						uint32_t next = (uint32_t)get_be(p + (i + 1) * 2, 2);
						if (next >= 0xdc00 && next < 0xe000) {
							unit = 0x10000 + ((unit - 0xd800) << 10) + (next - 0xdc00);
							i++;
						}
					}
					append_utf8(text, unit);
				}
				out = Plist::string(std::move(text));
			}
			return true;
		}
		case 0x8: {
			size_t width = (size_t)low + 1;
			if (pos + width > table_)
				return false;
			out = Plist::integer((int64_t)get_be(data_ + pos, width));
			return true;
		}
		case 0xa:
		case 0xc:
		case 0xd: {
			uint64_t length = 0;
			if (!length_at(pos, low, length))
				return false;
			uint64_t refs = high == 0xd ? length * 2 : length;
			if (refs > (table_ - pos) / ref_size_)
				return false;
			auto ref_at = [&](uint64_t i) {
				return get_be(data_ + pos + i * ref_size_, ref_size_);
			};
			if (high == 0xd) {
				std::map<std::string, Plist> dict;
				for (uint64_t i = 0; i < length; i++) {
					Plist key;
					Plist value;
					if (!object(ref_at(i), key, depth + 1) ||
					    !object(ref_at(length + i), value, depth + 1))
						return false;
					if (key.type == Plist::Type::String)
						dict.emplace(std::move(key.s), std::move(value));
				}
				out = Plist::dictionary(std::move(dict));
			} else {
				std::vector<Plist> array;
				array.reserve((size_t)length);
				for (uint64_t i = 0; i < length; i++) {
					Plist child;
					if (!object(ref_at(i), child, depth + 1))
						return false;
					array.push_back(std::move(child));
				}
				out = Plist::arr(std::move(array));
			}
			return true;
		}
		default:
			out = {};
			return true;
		}
	}

	const uint8_t *data_;
	size_t size_;
	size_t offset_size_ = 0;
	size_t ref_size_ = 0;
	uint64_t objects_ = 0;
	uint64_t table_ = 0;
};

/* XML property lists, which some receivers send instead of the binary form. */
class XmlReader {
public:
	XmlReader(const uint8_t *data, size_t size) : text_(reinterpret_cast<const char *>(data), size) {}

	bool read(Plist &out)
	{
		size_t start = text_.find("<plist");
		if (start == std::string::npos)
			return false;
		pos_ = text_.find('>', start);
		if (pos_ == std::string::npos)
			return false;
		pos_++;
		std::string tag;
		return next_tag(tag) && value(tag, out, 0);
	}

private:
	bool next_tag(std::string &tag)
	{
		while (true) {
			size_t open = text_.find('<', pos_);
			if (open == std::string::npos)
				return false;
			size_t close = text_.find('>', open);
			if (close == std::string::npos)
				return false;
			pos_ = close + 1;
			tag = text_.substr(open + 1, close - open - 1);
			if (!tag.empty() && (tag[0] == '?' || tag[0] == '!'))
				continue;
			return true;
		}
	}

	bool content(const std::string &name, std::string &out)
	{
		std::string end = "</" + name + ">";
		size_t close = text_.find(end, pos_);
		if (close == std::string::npos)
			return false;
		out = unescape(text_.substr(pos_, close - pos_));
		pos_ = close + end.size();
		return true;
	}

	static std::string unescape(const std::string &in)
	{
		std::string out;
		for (size_t i = 0; i < in.size(); i++) {
			if (in[i] != '&') {
				out.push_back(in[i]);
				continue;
			}
			size_t semi = in.find(';', i);
			if (semi == std::string::npos) {
				out.push_back(in[i]);
				continue;
			}
			std::string entity = in.substr(i + 1, semi - i - 1);
			if (entity == "lt")
				out.push_back('<');
			else if (entity == "gt")
				out.push_back('>');
			else if (entity == "amp")
				out.push_back('&');
			else if (entity == "quot")
				out.push_back('"');
			else if (entity == "apos")
				out.push_back('\'');
			else if (!entity.empty() && entity[0] == '#')
				append_utf8(out, (uint32_t)std::strtoul(entity.c_str() + 1 + (entity[1] == 'x'),
									nullptr, entity[1] == 'x' ? 16 : 10));
			i = semi;
		}
		return out;
	}

	static std::vector<uint8_t> base64(const std::string &in)
	{
		std::vector<uint8_t> out;
		uint32_t acc = 0;
		int bits = 0;
		for (char c : in) {
			int v = c >= 'A' && c <= 'Z'   ? c - 'A'
				: c >= 'a' && c <= 'z' ? c - 'a' + 26
				: c >= '0' && c <= '9' ? c - '0' + 52
				: c == '+'             ? 62
				: c == '/'             ? 63
						       : -1;
			if (v < 0)
				continue;
			acc = (acc << 6) | (uint32_t)v;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back((uint8_t)(acc >> bits));
			}
		}
		return out;
	}

	bool value(const std::string &tag, Plist &out, int depth)
	{
		if (depth > kMaxDepth)
			return false;
		std::string text;
		if (tag == "true/" || tag == "false/") {
			out = Plist::boolean(tag == "true/");
			return true;
		}
		if (tag == "dict/" || tag == "array/") {
			out = tag == "dict/" ? Plist::dictionary({}) : Plist::arr({});
			return true;
		}
		if (tag == "string/") {
			out = Plist::string("");
			return true;
		}
		if (tag == "string" || tag == "key") {
			if (!content(tag, text))
				return false;
			out = Plist::string(std::move(text));
			return true;
		}
		if (tag == "integer") {
			if (!content(tag, text))
				return false;
			out = Plist::integer(std::strtoll(text.c_str(), nullptr, 10));
			return true;
		}
		if (tag == "real") {
			if (!content(tag, text))
				return false;
			out = Plist::real(std::strtod(text.c_str(), nullptr));
			return true;
		}
		if (tag == "data") {
			if (!content(tag, text))
				return false;
			out = Plist::blob(base64(text));
			return true;
		}
		if (tag == "date") {
			if (!content(tag, text))
				return false;
			out = Plist::string(std::move(text));
			return true;
		}
		if (tag == "array") {
			std::vector<Plist> array;
			std::string child;
			while (next_tag(child) && child != "/array") {
				Plist item;
				if (!value(child, item, depth + 1))
					return false;
				array.push_back(std::move(item));
			}
			out = Plist::arr(std::move(array));
			return child == "/array";
		}
		if (tag == "dict") {
			std::map<std::string, Plist> dict;
			std::string child;
			while (next_tag(child) && child != "/dict") {
				if (child != "key" || !content("key", text))
					return false;
				std::string key = text;
				Plist item;
				if (!next_tag(child) || !value(child, item, depth + 1))
					return false;
				dict.emplace(std::move(key), std::move(item));
			}
			out = Plist::dictionary(std::move(dict));
			return child == "/dict";
		}
		return false;
	}

	std::string text_;
	size_t pos_ = 0;
};

} /* namespace */

Plist Plist::boolean(bool value)
{
	Plist plist;
	plist.type = Type::Bool;
	plist.b = value;
	return plist;
}

Plist Plist::integer(int64_t value)
{
	Plist plist;
	plist.type = Type::Int;
	plist.i = value;
	return plist;
}

Plist Plist::real(double value)
{
	Plist plist;
	plist.type = Type::Real;
	plist.r = value;
	return plist;
}

Plist Plist::string(std::string value)
{
	Plist plist;
	plist.type = Type::String;
	plist.s = std::move(value);
	return plist;
}

Plist Plist::blob(std::vector<uint8_t> value)
{
	Plist plist;
	plist.type = Type::Data;
	plist.data = std::move(value);
	return plist;
}

Plist Plist::arr(std::vector<Plist> value)
{
	Plist plist;
	plist.type = Type::Array;
	plist.array = std::move(value);
	return plist;
}

Plist Plist::dictionary(std::map<std::string, Plist> value)
{
	Plist plist;
	plist.type = Type::Dict;
	plist.dict = std::move(value);
	return plist;
}

const Plist *Plist::find(const std::string &key) const
{
	if (type != Type::Dict)
		return nullptr;
	auto it = dict.find(key);
	if (it == dict.end())
		return nullptr;
	return &it->second;
}

int64_t Plist::get_int(const std::string &key, int64_t fallback) const
{
	const Plist *value = find(key);
	if (!value)
		return fallback;
	if (value->type == Type::Int)
		return value->i;
	if (value->type == Type::Real)
		return (int64_t)value->r;
	if (value->type == Type::Bool)
		return value->b ? 1 : 0;
	return fallback;
}

double Plist::get_real(const std::string &key, double fallback) const
{
	const Plist *value = find(key);
	if (!value)
		return fallback;
	if (value->type == Type::Real)
		return value->r;
	if (value->type == Type::Int)
		return (double)value->i;
	return fallback;
}

std::string Plist::get_string(const std::string &key) const
{
	const Plist *value = find(key);
	if (!value || value->type != Type::String)
		return {};
	return value->s;
}

bool Plist::get_bool(const std::string &key, bool fallback) const
{
	const Plist *value = find(key);
	if (!value)
		return fallback;
	if (value->type == Type::Bool)
		return value->b;
	if (value->type == Type::Int)
		return value->i != 0;
	return fallback;
}

bool encode_bplist(const Plist &plist, std::vector<uint8_t> &out, std::string &error)
{
	BplistWriter writer;
	if (!writer.write(plist, out)) {
		error = "Could not encode a binary property list";
		return false;
	}
	return true;
}

bool decode_plist(const uint8_t *bytes, size_t len, Plist &out, std::string &error)
{
	if (!bytes || len == 0) {
		error = "Empty property list";
		return false;
	}
	bool ok = len >= 8 && std::memcmp(bytes, "bplist", 6) == 0 ? BplistReader(bytes, len).read(out)
								   : XmlReader(bytes, len).read(out);
	if (!ok) {
		error = "Could not read the property list";
		return false;
	}
	return true;
}

} /* namespace airplay */
