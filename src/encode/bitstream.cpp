/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "encode/bitstream.hpp"

#include <cstring>

namespace airplay {
namespace {

struct Nal {
	const uint8_t *data;
	size_t size;
};

std::vector<Nal> split_annexb(const uint8_t *data, size_t size)
{
	std::vector<Nal> nals;
	size_t i = 0;
	size_t start = SIZE_MAX;
	auto close = [&](size_t end) {
		if (start == SIZE_MAX)
			return;
		while (end > start && data[end - 1] == 0)
			end--;
		if (end > start)
			nals.push_back({data + start, end - start});
	};
	while (i + 2 < size) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			close(i);
			i += 3;
			start = i;
			continue;
		}
		i++;
	}
	close(size);
	return nals;
}

/* Reads RBSP bits, skipping emulation prevention bytes. */
class BitReader {
public:
	explicit BitReader(const std::vector<uint8_t> &nal, size_t header)
	{
		for (size_t i = header; i < nal.size(); i++) {
			if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0 && i > header + 1)
				continue;
			rbsp_.push_back(nal[i]);
		}
	}

	uint32_t bits(int count)
	{
		uint32_t value = 0;
		for (int i = 0; i < count; i++) {
			size_t byte = pos_ >> 3;
			if (byte >= rbsp_.size()) {
				failed_ = true;
				return 0;
			}
			value = (value << 1) | ((rbsp_[byte] >> (7 - (pos_ & 7))) & 1);
			pos_++;
		}
		return value;
	}

	void skip(size_t count) { pos_ += count; }

	uint32_t ue()
	{
		int zeros = 0;
		while (bits(1) == 0) {
			if (failed_ || ++zeros > 31)
				return 0;
		}
		return zeros ? (1u << zeros) - 1 + bits(zeros) : 0;
	}

	const uint8_t *at_byte() const { return rbsp_.data() + (pos_ >> 3); }
	bool has(size_t bytes) const { return (pos_ >> 3) + bytes <= rbsp_.size(); }
	bool ok() const { return !failed_ && (pos_ >> 3) <= rbsp_.size(); }

private:
	std::vector<uint8_t> rbsp_;
	size_t pos_ = 0;
	bool failed_ = false;
};

int nal_type(Codec codec, const uint8_t *nal)
{
	return codec == Codec::Hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
}

void put16(std::vector<uint8_t> &out, size_t value)
{
	out.push_back((uint8_t)(value >> 8));
	out.push_back((uint8_t)value);
}

void put32(std::vector<uint8_t> &out, size_t value)
{
	put16(out, value >> 16);
	put16(out, value & 0xffff);
}

void put_array(std::vector<uint8_t> &out, uint8_t type, const std::vector<uint8_t> &nal)
{
	out.push_back((uint8_t)(0x80 | type));
	put16(out, 1);
	put16(out, nal.size());
	out.insert(out.end(), nal.begin(), nal.end());
}

} /* namespace */

bool annexb_to_length_prefixed(Codec codec, const uint8_t *data, size_t size, std::vector<uint8_t> &out,
			       ParameterSets &sets)
{
	out.clear();
	bool found = false;
	const bool hevc = codec == Codec::Hevc;
	for (const Nal &nal : split_annexb(data, size)) {
		found = true;
		int type = nal_type(codec, nal.data);
		std::vector<uint8_t> *set = nullptr;
		if (hevc && type == 32)
			set = &sets.vps;
		else if ((hevc && type == 33) || (!hevc && type == 7))
			set = &sets.sps;
		else if ((hevc && type == 34) || (!hevc && type == 8))
			set = &sets.pps;
		if (set) {
			set->assign(nal.data, nal.data + nal.size);
			continue;
		}
		if ((hevc && type == 35) || (!hevc && type == 9))
			continue;
		put32(out, nal.size);
		out.insert(out.end(), nal.data, nal.data + nal.size);
	}
	return found;
}

bool build_avcc(const ParameterSets &sets, std::vector<uint8_t> &out)
{
	out.clear();
	if (sets.sps.size() < 4 || sets.pps.empty())
		return false;
	uint8_t profile = sets.sps[1];
	out = {1, profile, sets.sps[2], sets.sps[3], 0xff, 0xe1};
	put16(out, sets.sps.size());
	out.insert(out.end(), sets.sps.begin(), sets.sps.end());
	out.push_back(1);
	put16(out, sets.pps.size());
	out.insert(out.end(), sets.pps.begin(), sets.pps.end());
	if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
		BitReader sps(sets.sps, 4);
		sps.ue();
		uint32_t chroma = sps.ue();
		if (chroma == 3)
			sps.bits(1);
		uint32_t luma_depth = sps.ue();
		uint32_t chroma_depth = sps.ue();
		if (!sps.ok())
			return false;
		out.push_back((uint8_t)(0xfc | (chroma & 3)));
		out.push_back((uint8_t)(0xf8 | (luma_depth & 7)));
		out.push_back((uint8_t)(0xf8 | (chroma_depth & 7)));
		out.push_back(0);
	}
	return true;
}

bool build_hvcc(const ParameterSets &sets, std::vector<uint8_t> &out)
{
	out.clear();
	if (sets.vps.empty() || sets.sps.size() < 15 || sets.pps.empty())
		return false;
	BitReader sps(sets.sps, 2);
	sps.bits(4);
	uint32_t sub_layers = sps.bits(3);
	sps.bits(1);
	if (!sps.has(12))
		return false;
	uint8_t general[12];
	std::memcpy(general, sps.at_byte(), sizeof general);
	sps.skip(96);
	bool profile_present[8] = {};
	bool level_present[8] = {};
	for (uint32_t i = 0; i < sub_layers; i++) {
		profile_present[i] = sps.bits(1);
		level_present[i] = sps.bits(1);
	}
	if (sub_layers > 0)
		sps.skip(2 * (8 - sub_layers));
	for (uint32_t i = 0; i < sub_layers; i++) {
		if (profile_present[i])
			sps.skip(88);
		if (level_present[i])
			sps.skip(8);
	}
	sps.ue();
	uint32_t chroma = sps.ue();
	if (chroma == 3)
		sps.bits(1);
	sps.ue();
	sps.ue();
	if (sps.bits(1)) {
		for (int i = 0; i < 4; i++)
			sps.ue();
	}
	uint32_t luma_depth = sps.ue();
	uint32_t chroma_depth = sps.ue();
	if (!sps.ok())
		return false;

	out.push_back(1);
	out.insert(out.end(), general, general + sizeof general);
	out.push_back(0xf0);
	out.push_back(0);
	out.push_back(0xfc);
	out.push_back((uint8_t)(0xfc | (chroma & 3)));
	out.push_back((uint8_t)(0xf8 | (luma_depth & 7)));
	out.push_back((uint8_t)(0xf8 | (chroma_depth & 7)));
	put16(out, 0);
	/* VideoToolbox leaves temporalIdNested clear. Mac receivers accept that layout. */
	out.push_back((uint8_t)(((sub_layers + 1) & 7) << 3 | 3));
	out.push_back(3);
	put_array(out, 32, sets.vps);
	put_array(out, 33, sets.sps);
	put_array(out, 34, sets.pps);
	return true;
}

std::vector<uint8_t> image_description(Codec codec, uint32_t width, uint32_t height,
				       const std::vector<uint8_t> &config)
{
	if (config.empty())
		return {};
	const bool hevc = codec == Codec::Hevc;
	std::vector<uint8_t> out;
	put32(out, 0);
	const char *type = hevc ? "hvc1" : "avc1";
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), 6, 0);
	put16(out, 1);
	put16(out, 0);
	put16(out, 0);
	put32(out, 0);
	put32(out, 0x200);
	put32(out, 0x200);
	put16(out, width);
	put16(out, height);
	put32(out, 0x00480000);
	put32(out, 0x00480000);
	put32(out, 0);
	put16(out, 1);
	const char *name = hevc ? "HEVC" : "H.264";
	uint8_t compressor[32] = {};
	compressor[0] = (uint8_t)std::strlen(name);
	std::memcpy(compressor + 1, name, compressor[0]);
	out.insert(out.end(), compressor, compressor + sizeof compressor);
	put16(out, 0x18);
	put16(out, 0xffff);
	put32(out, config.size() + 8);
	const char *atom = hevc ? "hvcC" : "avcC";
	out.insert(out.end(), atom, atom + 4);
	out.insert(out.end(), config.begin(), config.end());
	put32(out, 0);
	size_t total = out.size();
	out[0] = (uint8_t)(total >> 24);
	out[1] = (uint8_t)(total >> 16);
	out[2] = (uint8_t)(total >> 8);
	out[3] = (uint8_t)total;
	return out;
}

} /* namespace airplay */
