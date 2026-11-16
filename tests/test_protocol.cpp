/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/aead.hpp"
#include "airplay/crypto.hpp"
#include "airplay/discovery.hpp"
#include "airplay/fairplay.hpp"
#include "airplay/features.hpp"
#include "airplay/handshake.hpp"
#include "airplay/mirror_packet.hpp"
#include "airplay/ntp.hpp"
#include "airplay/pairing.hpp"
#include "airplay/plist.hpp"
#include "airplay/rtp_audio.hpp"
#include "airplay/rtsp.hpp"
#include "encode/bitstream.hpp"
#include "cpp/fairplay_sender.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fairplay_vectors.inc"

namespace {

int g_failures = 0;

void check(bool ok, const char *text, int line)
{
	if (ok)
		return;
	std::fprintf(stderr, "FAIL %s:%d: %s\n", "tests/test_protocol.cpp", line, text);
	g_failures++;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

std::string hex_of(const uint8_t *data, size_t size)
{
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.resize(size * 2);
	for (size_t i = 0; i < size; i++) {
		out[i * 2] = digits[data[i] >> 4];
		out[i * 2 + 1] = digits[data[i] & 0xf];
	}
	return out;
}

std::vector<uint8_t> bytes_of(const char *hex)
{
	std::vector<uint8_t> out;
	airplay::hex_decode(hex, out);
	return out;
}

std::vector<uint8_t> rtsp_response(int status, const std::vector<uint8_t> &body)
{
	std::string head = "RTSP/1.0 " + std::to_string(status) + " OK\r\nCSeq: 1\r\n";
	if (!body.empty()) {
		head += "Content-Type: application/x-apple-binary-plist\r\nContent-Length: " +
			std::to_string(body.size()) + "\r\n";
	}
	head += "\r\n";
	std::vector<uint8_t> out(head.begin(), head.end());
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

std::vector<uint8_t> must_plist(const airplay::Plist &plist)
{
	std::vector<uint8_t> bytes;
	std::string error;
	if (!airplay::encode_bplist(plist, bytes, error))
		std::fprintf(stderr, "plist encode: %s\n", error.c_str());
	return bytes;
}

airplay::Plist info_plist(int64_t features)
{
	std::map<std::string, airplay::Plist> display;
	display.emplace("width", airplay::Plist::integer(1920));
	display.emplace("height", airplay::Plist::integer(1080));
	display.emplace("maxFPS", airplay::Plist::integer(60));
	std::map<std::string, airplay::Plist> root;
	root.emplace("features", airplay::Plist::integer(features));
	root.emplace("model", airplay::Plist::string("TestTV1,1"));
	root.emplace("deviceID", airplay::Plist::string("aa:bb:cc:dd:ee:ff"));
	root.emplace("displays", airplay::Plist::arr({airplay::Plist::dictionary(std::move(display))}));
	return airplay::Plist::dictionary(std::move(root));
}

struct ScriptedTransport : airplay::ByteTransport {
	std::string written;
	std::vector<std::vector<uint8_t>> replies;
	size_t index = 0;
	size_t offset = 0;

	bool write_all(const uint8_t *data, size_t size) override
	{
		written.append(reinterpret_cast<const char *>(data), size);
		return true;
	}

	int read_some(uint8_t *data, size_t capacity, int) override
	{
		if (index >= replies.size())
			return 0;
		std::vector<uint8_t> &reply = replies[index];
		size_t left = reply.size() - offset;
		size_t count = left < capacity ? left : capacity;
		std::memcpy(data, reply.data() + offset, count);
		offset += count;
		if (offset >= reply.size()) {
			index++;
			offset = 0;
		}
		return (int)count;
	}
};

airplay::HandshakeInput base_input()
{
	airplay::HandshakeInput input;
	input.receiver.address = "127.0.0.1";
	input.receiver.port = 7000;
	input.receiver.name = "Test TV";
	input.src_width = 1920;
	input.src_height = 1080;
	input.src_fps = 60;
	input.send_audio = false;
	input.timing_port = 7010;
	return input;
}

bool same_plist(const airplay::Plist &a, const airplay::Plist &b)
{
	if (a.type != b.type)
		return false;
	switch (a.type) {
	case airplay::Plist::Type::Null:
		return true;
	case airplay::Plist::Type::Bool:
		return a.b == b.b;
	case airplay::Plist::Type::Int:
		return a.i == b.i;
	case airplay::Plist::Type::Real:
		return a.r == b.r;
	case airplay::Plist::Type::String:
		return a.s == b.s;
	case airplay::Plist::Type::Data:
		return a.data == b.data;
	case airplay::Plist::Type::Array:
		if (a.array.size() != b.array.size())
			return false;
		for (size_t i = 0; i < a.array.size(); i++)
			if (!same_plist(a.array[i], b.array[i]))
				return false;
		return true;
	case airplay::Plist::Type::Dict:
		if (a.dict.size() != b.dict.size())
			return false;
		for (const auto &entry : a.dict) {
			const airplay::Plist *other = b.find(entry.first);
			if (!other || !same_plist(entry.second, *other))
				return false;
		}
		return true;
	}
	return false;
}

/* Round-trips through CoreFoundation, the encoder the plugin used before it had its own. */
void test_plist_against_cf()
{
	std::map<std::string, airplay::Plist> root;
	root.emplace("negative", airplay::Plist::integer(-42));
	root.emplace("big", airplay::Plist::integer(0x123456789abcdefll));
	root.emplace("word", airplay::Plist::integer(70000));
	root.emplace("rate", airplay::Plist::real(-0.125));
	root.emplace("name",
		     airplay::Plist::string("B\xc3\xbcro TV \xe2\x80\x93 \xe5\xae\xa2\xe5\x8e\x85 \xf0\x9f\x93\xba"));
	root.emplace("empty", airplay::Plist::string(""));
	std::vector<uint8_t> blob(300);
	for (size_t i = 0; i < blob.size(); i++)
		blob[i] = (uint8_t)(i * 7);
	root.emplace("blob", airplay::Plist::blob(blob));
	std::vector<airplay::Plist> many;
	for (int i = 0; i < 300; i++)
		many.push_back(airplay::Plist::integer(i * 1000));
	root.emplace("many", airplay::Plist::arr(many));
	std::map<std::string, airplay::Plist> wide;
	for (int i = 0; i < 20; i++)
		wide.emplace("key" + std::to_string(i), airplay::Plist::boolean(i % 2 == 0));
	root.emplace("wide", airplay::Plist::dictionary(wide));
	const airplay::Plist original = airplay::Plist::dictionary(root);

	std::vector<uint8_t> mine;
	std::string error;
	CHECK(airplay::encode_bplist(original, mine, error));
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, mine.data(), (CFIndex)mine.size());
	CFPropertyListRef parsed = CFPropertyListCreateWithData(kCFAllocatorDefault, data, 0, nullptr, nullptr);
	CFRelease(data);
	CHECK(parsed != nullptr);
	if (!parsed)
		return;
	const CFPropertyListFormat formats[] = {kCFPropertyListBinaryFormat_v1_0, kCFPropertyListXMLFormat_v1_0};
	for (CFPropertyListFormat format : formats) {
		CFDataRef written = CFPropertyListCreateData(kCFAllocatorDefault, parsed, format, 0, nullptr);
		CHECK(written != nullptr);
		if (!written)
			continue;
		airplay::Plist decoded;
		CHECK(airplay::decode_plist(CFDataGetBytePtr(written), (size_t)CFDataGetLength(written), decoded,
					    error));
		CHECK(same_plist(original, decoded));
		CFRelease(written);
	}
	CFRelease(parsed);
}

void test_features()
{
	uint64_t sample = airplay::parse_features_txt("0x40000a00,0x80300");
	CHECK(sample == 0x0008030040000a00ull);
	CHECK(airplay::parse_features_txt("0x40000a00, 0x80300") == sample);
	CHECK(!airplay::feature_bit(sample, 7));
	uint64_t screen = airplay::parse_features_txt("0x80");
	CHECK(airplay::feature_bit(screen, 7));
	CHECK(airplay::reject_reason(screen) == airplay::RejectReason::None);
	CHECK(airplay::reject_reason(1ull << 9) == airplay::RejectReason::AudioOnly);
	CHECK(airplay::reject_reason(1ull << 4) == airplay::RejectReason::VideoUrlOnly);
	CHECK(airplay::reject_reason(0) == airplay::RejectReason::NoScreen);
	CHECK(airplay::advertise_hevc(1ull << 42, {}) == true);
	airplay::DisplayMode mode;
	mode.hevc = true;
	CHECK(airplay::advertise_hevc(0, {mode}) == true);
	CHECK(airplay::advertise_hevc(1ull << 7, {}) == false);

	uint64_t office = airplay::parse_features_txt("0x7F8AD0,0x38BCF46");
	CHECK(office == 0x038BCF46007F8AD0ull);
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::Screen));
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::ScreenMultiCodec));
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::TransientPairing));
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::UnifiedPairing));
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::HomeKitPairing));
	CHECK(airplay::feature_bit(office, (int)airplay::FeatureBit::CoreUtilsPairing));
	CHECK(!airplay::feature_bit(office, (int)airplay::FeatureBit::FairPlayAesGcm));
	CHECK(!airplay::feature_bit(office, (int)airplay::FeatureBit::FairPlayAuth));
	CHECK(airplay::requires_pairing(office, 0x644));
	CHECK(airplay::requires_one_time_code(0x644));
	CHECK(!airplay::requires_one_time_code(0x40));
	CHECK(airplay::requires_one_time_code(0x200));
	CHECK(airplay::requires_one_time_code(0x204));
	CHECK(airplay::pin_hkp(office) == 3);
	const uint64_t mac = 0x38174FDE4A7FCFD5ull;
	CHECK(airplay::pin_hkp(mac) == 5);
	CHECK(airplay::feature_bit(mac, (int)airplay::FeatureBit::FairPlayAuth));
	CHECK(airplay::feature_bit(mac, (int)airplay::FeatureBit::TransientPairing));
	CHECK(!airplay::feature_bit(mac, (int)airplay::FeatureBit::UnifiedPairing));
	CHECK(airplay::pin_hkp((1ull << 7) | (1ull << 48)) == 5);
	CHECK(airplay::requires_fairplay(office, false));
	CHECK(!airplay::requires_pairing(1ull << 38, 0));
	CHECK(airplay::requires_fairplay(0, true));
	std::string pk(64, 'a');
	std::vector<uint8_t> wrapped;
	std::string error;
	uint8_t key[16] = {};
	CHECK(!airplay::rsa_oaep_wrap_key(pk, key, wrapped, error));
}

void test_choose_encode()
{
	airplay::EncodeConfig h264 = airplay::choose_encode(1920, 1080, 60, 1920, 1080, 60, false, 0);
	CHECK(h264.codec == airplay::Codec::H264);
	CHECK(h264.width == 1920);
	CHECK(h264.height == 1080);
	CHECK(h264.fps == 60);
	CHECK(h264.bitrate == 12000000);

	airplay::EncodeConfig hevc = airplay::choose_encode(1920, 1080, 60, 1920, 1080, 60, true, 0);
	CHECK(hevc.codec == airplay::Codec::Hevc);
	CHECK(hevc.bitrate == 7200000);

	airplay::EncodeConfig p720 = airplay::choose_encode(1280, 720, 60, 0, 0, 0, false, 0);
	CHECK(p720.width == 1280);
	CHECK(p720.height == 720);
	CHECK(p720.bitrate == 5333333);

	airplay::EncodeConfig fit = airplay::choose_encode(1920, 1080, 60, 1280, 720, 30, false, 0);
	CHECK(fit.width == 1280);
	CHECK(fit.height == 720);
	CHECK(fit.fps == 30);

	airplay::EncodeConfig odd = airplay::choose_encode(1919, 1081, 60, 0, 0, 0, false, 0);
	CHECK(odd.width == 1918);
	CHECK(odd.height == 1080);

	airplay::EncodeConfig capped = airplay::choose_encode(1920, 1080, 60, 1920, 1080, 24, false, 1000);
	CHECK(capped.fps == 24);
	CHECK(capped.bitrate == 1000);
}

void test_mirror_and_ntp()
{
	const uint8_t payload[] = {0x01, 0x64, 0xc0, 0x28, 0xff, 0xe1, 0x00, 0x1f, 0x67, 0x64, 0xc0,
				   0x28, 0xac, 0x2b, 0x40, 0x1e, 0x00, 0x8d, 0x68, 0x07, 0x88, 0x00,
				   0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xf0};
	CHECK(sizeof payload == 31);
	auto packet =
		airplay::build_mirror_packet(airplay::MirrorPayloadType::Codec, 0, 864, 480, payload, sizeof payload);
	CHECK(packet.size() == 128 + 31);
	CHECK(packet[0] == 31 && packet[1] == 0 && packet[2] == 0 && packet[3] == 0);
	CHECK(packet[4] == 1 && packet[5] == 0);
	CHECK(packet[6] == 0x16 && packet[7] == 0x01);
	CHECK(std::memcmp(packet.data() + 128, payload, sizeof payload) == 0);
	float width = 0;
	uint32_t bits = (uint32_t)packet[16] | ((uint32_t)packet[17] << 8) | ((uint32_t)packet[18] << 16) |
			((uint32_t)packet[19] << 24);
	std::memcpy(&width, &bits, 4);
	CHECK(width == 864.f);
	uint32_t dest_bits = (uint32_t)packet[56] | ((uint32_t)packet[57] << 8) | ((uint32_t)packet[58] << 16) |
			     ((uint32_t)packet[59] << 24);
	float dest = 0;
	std::memcpy(&dest, &dest_bits, 4);
	CHECK(dest == 864.f);

	const uint8_t nal[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
	auto idr = airplay::build_mirror_packet(airplay::MirrorPayloadType::Video, 0, 864, 480, nal, sizeof nal, true);
	CHECK(idr.size() == 128 + sizeof nal);
	CHECK(idr[4] == 0 && idr[5] == 0x10 && idr[6] == 0 && idr[7] == 0);
	CHECK(idr[16] == 0 && idr[40] == 0);
	auto delta =
		airplay::build_mirror_packet(airplay::MirrorPayloadType::Video, 0, 864, 480, nal, sizeof nal, false);
	CHECK(delta[4] == 0 && delta[5] == 0 && delta[6] == 0);

	auto hevc = airplay::build_mirror_packet(airplay::MirrorPayloadType::Codec, 0, 864, 480, payload,
						 sizeof payload, false, true);
	CHECK(hevc.size() == 128 + 31);
	CHECK(hevc[6] == 0x1e && hevc[7] == 0x01);

	auto beat = airplay::build_mirror_packet(airplay::MirrorPayloadType::Heartbeat, 0, 864, 480, nullptr, 0);
	CHECK(beat.size() == 128);
	CHECK(beat[0] == 0 && beat[4] == 2 && beat[6] == 0x1e && beat[7] == 0);

	uint8_t request[48] = {};
	request[0] = 0x23;
	request[40] = 0x11;
	request[47] = 0x22;
	uint64_t now = 0x0000006400000001ull;
	auto reply = airplay::ntp_reply(request, 48, now);
	CHECK(reply.size() == 48);
	CHECK(reply[0] == 0x24);
	CHECK(reply[1] == 1);
	CHECK(reply[12] == 'A' && reply[13] == 'I' && reply[14] == 'R' && reply[15] == 'P');
	CHECK(std::memcmp(reply.data() + 24, request + 40, 8) == 0);
	CHECK(reply[32] == 0x00 && reply[33] == 0x00 && reply[34] == 0x00 && reply[35] == 0x64);
	CHECK(airplay::ntp_reply(request, 47, now).empty());
	CHECK(airplay::ntp_reply(nullptr, 48, now).empty());

	uint8_t timing[32] = {};
	timing[0] = 0x80;
	timing[1] = 0xd2;
	timing[24] = 0x11;
	timing[31] = 0x22;
	auto timing_reply = airplay::airplay_timing_reply(timing, 32, now);
	CHECK(timing_reply.size() == 32);
	CHECK(timing_reply[0] == 0x80 && timing_reply[1] == 0xd3);
	CHECK(timing_reply[8] == 0x11 && timing_reply[15] == 0x22);
	/* 2208988800 + 0x64 seconds, fraction 1, big-endian. */
	CHECK(timing_reply[16] == 0x83 && timing_reply[17] == 0xaa && timing_reply[18] == 0x7e &&
	      timing_reply[19] == 0xe4);
	CHECK(timing_reply[23] == 0x01);
	CHECK(airplay::airplay_timing_reply(timing, 31, now).empty());
	timing[1] = 0xd3;
	CHECK(airplay::airplay_timing_reply(timing, 32, now).empty());
}

void test_rtsp_plist_rtp()
{
	const char *raw = "RTSP/1.0 200 OK\r\nCSeq: 3\r\nContent-Length: 4\r\n\r\npingNEXT";
	airplay::RtspMessage message;
	size_t consumed = 0;
	CHECK(airplay::parse_rtsp_message(reinterpret_cast<const uint8_t *>(raw), std::strlen(raw), message, consumed));
	CHECK(message.status == 200);
	CHECK(airplay::header_get(message, "cseq") == "3");
	CHECK(message.body.size() == 4);
	CHECK(consumed + 4 == std::strlen(raw));

	std::map<std::string, airplay::Plist> nested;
	nested.emplace("name", airplay::Plist::string("txtAirPlay"));
	nested.emplace("count", airplay::Plist::integer(7));
	nested.emplace("rate", airplay::Plist::real(59.94));
	nested.emplace("on", airplay::Plist::boolean(true));
	std::vector<uint8_t> blob = {1, 2, 3, 4};
	nested.emplace("blob", airplay::Plist::blob(blob));
	std::map<std::string, airplay::Plist> root;
	root.emplace("qualifier", airplay::Plist::arr({airplay::Plist::dictionary(nested)}));
	std::vector<uint8_t> encoded;
	std::string error;
	CHECK(airplay::encode_bplist(airplay::Plist::dictionary(root), encoded, error));
	airplay::Plist decoded;
	CHECK(airplay::decode_plist(encoded.data(), encoded.size(), decoded, error));
	const airplay::Plist *qualifier = decoded.find("qualifier");
	CHECK(qualifier && qualifier->type == airplay::Plist::Type::Array && qualifier->array.size() == 1);
	CHECK(qualifier->array[0].get_string("name") == "txtAirPlay");
	CHECK(qualifier->array[0].get_int("count") == 7);
	CHECK(qualifier->array[0].get_bool("on"));
	CHECK(qualifier->array[0].find("blob") && qualifier->array[0].find("blob")->data.size() == 4);

	test_plist_against_cf();

	uint8_t payload[] = {9, 8, 7};
	auto rtp = airplay::build_rtp_audio(0x1234, 0x01020304, 0x41504d31, payload, sizeof payload, true);
	CHECK(rtp.size() == 15);
	CHECK(rtp[0] == 0x80);
	CHECK(rtp[1] == (0x80 | 96));
	CHECK(rtp[2] == 0x12 && rtp[3] == 0x34);
	CHECK(rtp[4] == 0x01 && rtp[5] == 0x02 && rtp[6] == 0x03 && rtp[7] == 0x04);
	CHECK(rtp[8] == 0x41 && rtp[11] == 0x31);
	CHECK(rtp[12] == 9 && rtp[14] == 7);
}

void test_audio_setup()
{
	std::vector<uint8_t> shk(32, 0x5a);
	std::vector<uint8_t> body;
	std::string error;
	CHECK(airplay::audio_setup_body(1234, shk, 6001, body, error));
	airplay::Plist root;
	CHECK(airplay::decode_plist(body.data(), body.size(), root, error));
	const airplay::Plist *streams = root.find("streams");
	CHECK(streams && streams->type == airplay::Plist::Type::Array && streams->array.size() == 1);
	if (streams && streams->array.size() == 1) {
		const airplay::Plist &audio = streams->array[0];
		CHECK(audio.get_int("type") == 96);
		CHECK(audio.get_int("streamConnectionID") == 1234);
		CHECK(audio.find("shk") && audio.find("shk")->data == shk);
		CHECK(audio.get_int("ct") == 8);
		CHECK(audio.get_int("spf") == 480);
		CHECK(audio.get_int("sr") == 48000);
		CHECK(audio.get_int("audioFormat") == 0x2000000);
		CHECK(audio.get_string("audioMode") == "default");
		CHECK(audio.get_bool("usingScreen"));
		CHECK(!audio.get_bool("isMedia", true));
		CHECK(audio.get_int("controlPort") == 6001);
		CHECK(audio.get_int("latencyMin") == 8160);
		CHECK(audio.get_int("latencyMax") == 96000);
		CHECK(!audio.find("streamConnections"));
	}
	CHECK(airplay::audio_setup_body(1, {}, 6001, body, error));
	CHECK(airplay::decode_plist(body.data(), body.size(), root, error));
	streams = root.find("streams");
	CHECK(streams && streams->array.size() == 1 && !streams->array[0].find("shk"));

	auto sync = airplay::classic_sync_packet(0x01020304, 0x0a0b0c0d, 0x1122334455667788ull, true);
	CHECK(sync.size() == 20);
	CHECK(sync[0] == 0x90 && sync[1] == 0xd4 && sync[2] == 0 && sync[3] == 4);
	CHECK(sync[4] == 0x01 && sync[7] == 0x04);
	CHECK(sync[8] == 0x11 && sync[15] == 0x88);
	CHECK(sync[16] == 0x0a && sync[19] == 0x0d);
	CHECK(airplay::classic_sync_packet(0, 0, 0, false)[0] == 0x80);

	constexpr uint64_t kMs = 1000000ull;
	constexpr uint32_t kRate = 48000;
	airplay::AudioSyncClock clock;
	/* The first packet was captured 30 ms before it left. */
	CHECK(clock.due(48000, 1000 * kMs, 1030 * kMs, kRate) == 48000 + 1440);
	/* A packet whose capture time is off by 20 ms does not move the clock. */
	CHECK(clock.due(48000 + 4800, 1120 * kMs, 1500 * kMs, kRate) == 48000 + 24000);
	/* No capture time, or one over a second old, counts as captured now. */
	CHECK(clock.due(48000 + 48000, 0, 2000 * kMs, kRate) == 48000 + 48000);
	/* A jump of over a second starts a new clock from that packet. */
	CHECK(clock.due(500000, 2100 * kMs, 2110 * kMs, kRate) == 500000 + 480);
	CHECK(clock.due(500000 + 480, 0, 2120 * kMs, kRate) == 500000 + 960);
	/* RTP times wrap. */
	airplay::AudioSyncClock wrap;
	CHECK(wrap.due(0xffffff00u, 5000 * kMs, 5000 * kMs, kRate) == 0xffffff00u);
	CHECK(wrap.due(0xffffff00u + 480, 0, 5010 * kMs, kRate) == 0xffffff00u + 480);
}

void test_crypto()
{
	uint8_t key[16];
	uint8_t iv[16];
	for (int i = 0; i < 16; i++) {
		key[i] = (uint8_t)(i + 1);
		iv[i] = (uint8_t)(0xa0 + i);
	}
	uint8_t plain[32];
	for (int i = 0; i < 32; i++)
		plain[i] = (uint8_t)i;
	uint8_t first[32];
	std::memcpy(first, plain, sizeof first);
	airplay::AesCtr encrypt;
	std::string error;
	CHECK(encrypt.set_key(key, iv, error));
	CHECK(encrypt.process(first, sizeof first, error));
	CHECK(std::memcmp(first, plain, sizeof first) != 0);
	airplay::AesCtr decrypt;
	CHECK(decrypt.set_key(key, iv, error));
	CHECK(decrypt.process(first, sizeof first, error));
	CHECK(std::memcmp(first, plain, sizeof first) == 0);

	/* NIST SP 800-38A F.5.1, fed in uneven pieces so the keystream must carry across calls. */
	std::vector<uint8_t> nist_key = bytes_of("2b7e151628aed2a6abf7158809cf4f3c");
	std::vector<uint8_t> nist_iv = bytes_of("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
	std::vector<uint8_t> nist = bytes_of("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
	airplay::AesCtr ctr;
	CHECK(ctr.set_key(nist_key.data(), nist_iv.data(), error));
	CHECK(ctr.process(nist.data(), 5, error));
	CHECK(ctr.process(nist.data() + 5, nist.size() - 5, error));
	CHECK(nist == bytes_of("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"));

	uint8_t digest[64];
	airplay::sha512(reinterpret_cast<const uint8_t *>("abc"), 3, digest);
	CHECK(hex_of(digest, 64) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
				    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
	std::string long_text(1000, 'a');
	airplay::sha512(reinterpret_cast<const uint8_t *>(long_text.data()), long_text.size(), digest);
	CHECK(hex_of(digest, 64) == "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634"
				    "fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");
	/* RFC 4231 test case 1. */
	std::vector<uint8_t> hmac_key(20, 0x0b);
	airplay::hmac_sha512(hmac_key.data(), hmac_key.size(), reinterpret_cast<const uint8_t *>("Hi There"), 8,
			     digest);
	CHECK(hex_of(digest, 64) == "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
				    "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
	uint8_t sha1_digest[20];
	airplay::sha1(reinterpret_cast<const uint8_t *>("abc"), 3, sha1_digest);
	CHECK(hex_of(sha1_digest, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");

	std::vector<uint8_t> short_key(32, 0x11);
	std::vector<uint8_t> wrapped;
	CHECK(!airplay::rsa_oaep_wrap_key(hex_of(short_key.data(), short_key.size()), key, wrapped, error));

	CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
								 &kCFTypeDictionaryValueCallBacks);
	int bits = 2048;
	CFNumberRef bits_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits);
	CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeRSA);
	CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, bits_number);
	CFErrorRef cf_error = nullptr;
	SecKeyRef private_key = SecKeyCreateRandomKey(attrs, &cf_error);
	CFRelease(bits_number);
	CFRelease(attrs);
	CHECK(private_key != nullptr);
	if (!private_key)
		return;
	SecKeyRef public_key = SecKeyCopyPublicKey(private_key);
	CFDataRef external = SecKeyCopyExternalRepresentation(public_key, &cf_error);
	CHECK(external != nullptr);
	if (external) {
		auto *bytes = CFDataGetBytePtr(external);
		CFIndex length = CFDataGetLength(external);
		CHECK(length > 0 && bytes[0] == 0x30);
		CHECK(airplay::rsa_oaep_wrap_key(hex_of(bytes, (size_t)length), key, wrapped, error));
		CFDataRef cipher = CFDataCreate(kCFAllocatorDefault, wrapped.data(), (CFIndex)wrapped.size());
		CFDataRef opened = SecKeyCreateDecryptedData(private_key, kSecKeyAlgorithmRSAEncryptionOAEPSHA1, cipher,
							     &cf_error);
		CHECK(opened != nullptr);
		if (opened) {
			CHECK(CFDataGetLength(opened) == 16);
			CHECK(std::memcmp(CFDataGetBytePtr(opened), key, 16) == 0);
			CFRelease(opened);
		}
		CFRelease(cipher);
		CFRelease(external);
	}
	CFRelease(public_key);
	CFRelease(private_key);
}

void test_handshake()
{
	const int64_t hevc_features = (int64_t)((1ull << 7) | (1ull << 42));
	const int64_t h264_features = (int64_t)(1ull << 7);
	std::vector<uint8_t> session = must_plist([] {
		std::map<std::string, airplay::Plist> body;
		body.emplace("dataPort", airplay::Plist::integer(7020));
		return airplay::Plist::dictionary(std::move(body));
	}());
	std::vector<uint8_t> video = must_plist([] {
		std::map<std::string, airplay::Plist> body;
		body.emplace("dataPort", airplay::Plist::integer(0));
		return airplay::Plist::dictionary(std::move(body));
	}());

	ScriptedTransport ok;
	ok.replies.push_back(rtsp_response(200, must_plist(info_plist(hevc_features))));
	std::vector<uint8_t> combined = rtsp_response(200, session);
	std::vector<uint8_t> video_response = rtsp_response(200, video);
	combined.insert(combined.end(), video_response.begin(), video_response.end());
	ok.replies.push_back(std::move(combined));
	ok.replies.push_back(rtsp_response(200, {}));
	airplay::HandshakeResult result = airplay::handshake(ok, base_input());
	CHECK(result.ok);
	CHECK(result.video_port == 7020);
	CHECK(result.config.codec == airplay::Codec::Hevc);
	CHECK(result.max_width == 1920);
	CHECK(result.max_fps == 60);
	CHECK(!result.encrypt);
	CHECK(ok.written.find("GET ") != std::string::npos);
	CHECK(ok.written.find("RECORD ") != std::string::npos);
	CHECK(ok.written.find("fp-setup") == std::string::npos);
	CHECK(ok.written.find("pair-setup") == std::string::npos);
	CHECK(ok.written.find("HEVC") != std::string::npos);
	CHECK(ok.written.find("audioFormat") == std::string::npos);
	CHECK(ok.written.find("User-Agent: AirPlay/960.13.1") != std::string::npos);

	std::vector<uint8_t> audio_reply = must_plist([] {
		std::map<std::string, airplay::Plist> stream;
		stream.emplace("type", airplay::Plist::integer(96));
		stream.emplace("dataPort", airplay::Plist::integer(7030));
		stream.emplace("controlPort", airplay::Plist::integer(7031));
		std::map<std::string, airplay::Plist> body;
		body.emplace("streams", airplay::Plist::arr({airplay::Plist::dictionary(std::move(stream))}));
		return airplay::Plist::dictionary(std::move(body));
	}());
	airplay::HandshakeInput audio_input = base_input();
	audio_input.send_audio = true;
	ScriptedTransport with_audio;
	with_audio.replies.push_back(rtsp_response(200, must_plist(info_plist(hevc_features))));
	with_audio.replies.push_back(rtsp_response(200, session));
	with_audio.replies.push_back(rtsp_response(200, video));
	with_audio.replies.push_back(rtsp_response(200, {}));
	with_audio.replies.push_back(rtsp_response(200, audio_reply));
	airplay::HandshakeResult heard = airplay::handshake(with_audio, audio_input);
	CHECK(heard.ok);
	CHECK(heard.audio_negotiated);
	CHECK(heard.audio_port == 7030);
	CHECK(heard.audio_control_port == 7031);
	/* A Mac receiver only reads its audio sockets once RECORD has run. */
	CHECK(heard.requests.size() >= 2 && heard.requests[heard.requests.size() - 2] == "RECORD /stream");
	CHECK(!heard.requests.empty() && heard.requests.back() == "SETUP audio");

	ScriptedTransport refused;
	refused.replies.push_back(rtsp_response(200, must_plist(info_plist(hevc_features))));
	refused.replies.push_back(rtsp_response(200, session));
	refused.replies.push_back(rtsp_response(200, video));
	refused.replies.push_back(rtsp_response(200, {}));
	refused.replies.push_back(rtsp_response(400, {}));
	airplay::HandshakeResult silent = airplay::handshake(refused, audio_input);
	CHECK(silent.ok);
	CHECK(!silent.audio_negotiated);
	CHECK(silent.status.find("video only") != std::string::npos);

	ScriptedTransport denied;
	airplay::HandshakeInput pin_input = base_input();
	pin_input.pin = "2468";
	denied.replies.push_back(rtsp_response(403, {}));
	airplay::HandshakeResult auth = airplay::handshake(denied, pin_input);
	CHECK(!auth.ok);
	CHECK(auth.error.find("403") != std::string::npos);
	CHECK(auth.error.find("FairPlay") == std::string::npos);
	CHECK(auth.error.find("2468") == std::string::npos);
	CHECK(denied.written.find("GET ") != std::string::npos);
	CHECK(denied.written.find("RECORD ") == std::string::npos);

	ScriptedTransport fallback;
	fallback.replies.push_back(rtsp_response(200, must_plist(info_plist(hevc_features))));
	fallback.replies.push_back(rtsp_response(200, session));
	fallback.replies.push_back(rtsp_response(415, {}));
	fallback.replies.push_back(rtsp_response(200, must_plist(info_plist(h264_features))));
	fallback.replies.push_back(rtsp_response(200, session));
	fallback.replies.push_back(rtsp_response(200, video));
	fallback.replies.push_back(rtsp_response(200, {}));
	airplay::HandshakeResult second = airplay::handshake(fallback, base_input());
	CHECK(second.ok);
	CHECK(second.config.codec == airplay::Codec::H264);
	CHECK(second.config.fell_back_from_hevc);
	CHECK(second.video_port == 7020);
	CHECK(fallback.written.find("HEVC") != std::string::npos);
	CHECK(fallback.written.find("H264") != std::string::npos);
}

bool hook_m1(void *, std::vector<uint8_t> &out, std::string &)
{
	out.assign(16, 0);
	out[0] = 'F';
	out[1] = 'P';
	out[2] = 'L';
	out[3] = 'Y';
	return true;
}

bool hook_m3(void *, const std::vector<uint8_t> &, std::vector<uint8_t> &out, std::string &)
{
	out.assign(164, 0);
	out[0] = 'F';
	out[1] = 'P';
	out[2] = 'L';
	out[3] = 'Y';
	return true;
}

bool hook_wrap(void *, const std::vector<uint8_t> &, const std::vector<uint8_t> &, const uint8_t *,
	       std::vector<uint8_t> &ekey, std::string &)
{
	ekey.assign(72, 0x11);
	return true;
}

std::vector<uint8_t> fply_body(size_t size)
{
	std::vector<uint8_t> body(size, 0);
	body[0] = 'F';
	body[1] = 'P';
	body[2] = 'L';
	body[3] = 'Y';
	return body;
}

void test_fairplay_and_pairing()
{
	CHECK(airplay::aead_self_test());
	CHECK(airplay::srp_self_test());
	airplay::fairplay_install_hooks(nullptr);
	const int64_t fairplay_features = (int64_t)((1ull << 7) | (1ull << 14));
	ScriptedTransport plain;
	plain.replies.push_back(rtsp_response(200, must_plist(info_plist(fairplay_features))));
	plain.replies.push_back(rtsp_response(200, fply_body(142)));
	plain.replies.push_back(rtsp_response(200, fply_body(32)));
	std::vector<uint8_t> session = must_plist([] {
		std::map<std::string, airplay::Plist> body;
		body.emplace("dataPort", airplay::Plist::integer(7020));
		return airplay::Plist::dictionary(std::move(body));
	}());
	std::vector<uint8_t> video = must_plist([] {
		std::map<std::string, airplay::Plist> body;
		body.emplace("dataPort", airplay::Plist::integer(0));
		return airplay::Plist::dictionary(std::move(body));
	}());
	plain.replies.push_back(rtsp_response(200, session));
	plain.replies.push_back(rtsp_response(200, video));
	plain.replies.push_back(rtsp_response(200, {}));
	airplay::FairPlayHooks hooks{hook_m1, hook_m3, hook_wrap, nullptr};
	airplay::fairplay_install_hooks(&hooks);
	airplay::HandshakeInput fair = base_input();
	fair.receiver.features = (uint64_t)fairplay_features;
	fair.pin = "2468";
	airplay::HandshakeResult fair_result = airplay::handshake(plain, fair);
	CHECK(fair_result.ok);
	CHECK(fair_result.encrypt);
	CHECK(!fair_result.use_chacha);
	CHECK(fair_result.aes_key.size() == 16);
	CHECK(plain.written.find("fp-setup") != std::string::npos);
	CHECK(plain.written.find("X-Apple-ET: 32") != std::string::npos);
	CHECK(plain.written.find("pair-setup") == std::string::npos);
	CHECK(plain.written.find("2468") == std::string::npos);
	CHECK(fair_result.requests.size() >= 6);
	CHECK(fair_result.requests[0] == "GET /info");
	CHECK(fair_result.requests[1] == "POST /fp-setup");
	CHECK(fair_result.requests[2] == "POST /fp-setup");
	CHECK(fair_result.requests[3] == "SETUP session");
	CHECK(fair_result.requests[4] == "SETUP video");
	CHECK(fair_result.requests.back() == "RECORD /stream");

	airplay::fairplay_install_hooks(nullptr);
	ScriptedTransport unavailable;
	unavailable.replies.push_back(rtsp_response(200, must_plist(info_plist(fairplay_features))));
	airplay::HandshakeInput missing = base_input();
	missing.receiver.features = (uint64_t)fairplay_features;
	missing.pin = "2468";
	airplay::HandshakeResult missing_result = airplay::handshake(unavailable, missing);
	CHECK(!missing_result.ok);
	CHECK(missing_result.error.find("2468") == std::string::npos);
	CHECK(missing_result.error.find("FairPlay") != std::string::npos);
	bool saw_setup = false;
	for (const std::string &step : missing_result.requests) {
		if (step.find("SETUP") != std::string::npos)
			saw_setup = true;
	}
	CHECK(!saw_setup);

	const int64_t transient = (int64_t)((1ull << 7) | (1ull << 48));
	ScriptedTransport needs_code;
	needs_code.replies.push_back(rtsp_response(200, must_plist(info_plist(transient))));
	needs_code.replies.push_back(rtsp_response(200, {}));
	airplay::HandshakeInput empty_pin = base_input();
	empty_pin.receiver.features = (uint64_t)transient;
	empty_pin.receiver.status_flags = 0x644;
	bool asked_before_display = false;
	empty_pin.request_pin = [&]() {
		asked_before_display = needs_code.written.find("/pair-pin-start") == std::string::npos;
		return std::string();
	};
	airplay::HandshakeResult asked = airplay::handshake(needs_code, empty_pin);
	CHECK(!asked.ok);
	CHECK(!asked_before_display);
	CHECK(asked.error.find("Enter the pairing code") != std::string::npos);
	CHECK(needs_code.written.find("/pair-pin-start") != std::string::npos);
	CHECK(needs_code.written.find("X-Apple-SupportedPINLengths: 4") != std::string::npos);
	CHECK(needs_code.written.find("X-Apple-HKP: 5") != std::string::npos);
	CHECK(needs_code.written.find("X-Apple-Client-Name: ") != std::string::npos);
	CHECK(needs_code.written.find("X-Apple-Client-Name: \r\n") == std::string::npos);
	CHECK(needs_code.written.find("pair-setup") == std::string::npos);
	CHECK(needs_code.written.find("SETUP ") == std::string::npos);
	CHECK(asked.requests.size() == 2);
	CHECK(asked.requests[0] == "GET /info");
	CHECK(asked.requests[1] == "POST /pair-pin-start");

	ScriptedTransport no_display;
	no_display.replies.push_back(rtsp_response(200, must_plist(info_plist(transient))));
	no_display.replies.push_back(rtsp_response(404, {}));
	no_display.replies.push_back(rtsp_response(404, {}));
	no_display.replies.push_back(rtsp_response(404, {}));
	airplay::HandshakeInput hidden = base_input();
	hidden.receiver.features = (uint64_t)transient;
	hidden.receiver.status_flags = 0x644;
	bool prompted = false;
	hidden.request_pin = [&]() {
		prompted = true;
		return std::string("1357");
	};
	airplay::HandshakeResult hidden_result = airplay::handshake(no_display, hidden);
	CHECK(!hidden_result.ok);
	CHECK(!prompted);
	CHECK(hidden_result.error.find("did not show a pairing code") != std::string::npos);
	CHECK(hidden_result.error.find("1357") == std::string::npos);
	CHECK(no_display.written.find("pair-setup") == std::string::npos);

	airplay::TlvItem salt_item;
	salt_item.type = (uint8_t)airplay::TlvType::Salt;
	salt_item.value.assign(16, 0x22);
	airplay::TlvItem key_item;
	key_item.type = (uint8_t)airplay::TlvType::PublicKey;
	key_item.value.assign(384, 0x11);
	airplay::TlvItem state_item;
	state_item.type = (uint8_t)airplay::TlvType::State;
	state_item.value.push_back(2);
	std::vector<uint8_t> m2 = airplay::tlv_encode({state_item, salt_item, key_item});
	airplay::TlvItem m4_state;
	m4_state.type = (uint8_t)airplay::TlvType::State;
	m4_state.value.push_back(4);
	airplay::TlvItem m4_error;
	m4_error.type = (uint8_t)airplay::TlvType::Error;
	m4_error.value.push_back(airplay::kTlvErrorAuthentication);
	std::vector<uint8_t> m4 = airplay::tlv_encode({m4_state, m4_error});

	ScriptedTransport rejected;
	rejected.replies.push_back(rtsp_response(200, must_plist(info_plist(transient))));
	rejected.replies.push_back(rtsp_response(404, {}));
	rejected.replies.push_back(rtsp_response(200, m2));
	rejected.replies.push_back(rtsp_response(200, m4));
	airplay::HandshakeInput wrong = base_input();
	wrong.receiver.features = (uint64_t)transient;
	wrong.receiver.status_flags = 0x644;
	wrong.pin = "2468";
	airplay::HandshakeResult bad = airplay::handshake(rejected, wrong);
	CHECK(!bad.ok);
	CHECK(bad.error.find("rejected") != std::string::npos);
	CHECK(bad.error.find("2468") == std::string::npos);
	CHECK(rejected.written.find("2468") == std::string::npos);
	CHECK(rejected.written.find("X-Apple-HKP: 5") != std::string::npos);
	CHECK(rejected.written.find("X-Apple-HKP: 4") == std::string::npos);
	CHECK(rejected.written.find("X-Apple-HKP: 3") == std::string::npos);
	auto pair_body = [](const std::string &written) {
		auto at = written.find("/pair-setup");
		auto sep = written.find("\r\n\r\n", at);
		if (at == std::string::npos || sep == std::string::npos)
			return std::string();
		return written.substr(sep + 4, 6);
	};
	const std::string pin_m1("\x00\x01\x00\x06\x01\x01", 6);
	const std::string transient_flag("\x13\x01\x10", 3);
	CHECK(pair_body(rejected.written) == pin_m1);
	CHECK(rejected.written.find(transient_flag) == std::string::npos);
	CHECK(rejected.written.find("fp-setup") == std::string::npos);
	int pair_posts = 0;
	bool setup = false;
	for (const std::string &step : bad.requests) {
		if (step == "POST /pair-setup")
			pair_posts++;
		if (step.find("SETUP") != std::string::npos)
			setup = true;
	}
	CHECK(pair_posts == 2);
	CHECK(!setup);

	const uint64_t office_features = 0x038BCF46007F8AD0ull;
	ScriptedTransport office_tv;
	office_tv.replies.push_back(rtsp_response(200, must_plist(info_plist((int64_t)office_features))));
	office_tv.replies.push_back(rtsp_response(404, {}));
	office_tv.replies.push_back(rtsp_response(200, m2));
	office_tv.replies.push_back(rtsp_response(200, m4));
	airplay::HandshakeInput office_pin = base_input();
	office_pin.receiver.features = office_features;
	office_pin.receiver.status_flags = 0x644;
	office_pin.pin = "2468";
	airplay::HandshakeResult office_bad = airplay::handshake(office_tv, office_pin);
	CHECK(!office_bad.ok);
	CHECK(office_bad.error.find("rejected") != std::string::npos);
	CHECK(office_bad.error.find("2468") == std::string::npos);
	CHECK(office_tv.written.find("X-Apple-HKP: 3") != std::string::npos);
	CHECK(office_tv.written.find("X-Apple-HKP: 5") == std::string::npos);
	CHECK(office_tv.written.find("X-Apple-HKP: 4") == std::string::npos);
	CHECK(pair_body(office_tv.written) == pin_m1);
	CHECK(office_tv.written.find(transient_flag) == std::string::npos);

	const uint64_t mac_features = 0x38174FDE4A7FCFD5ull;
	ScriptedTransport mac_tv;
	mac_tv.replies.push_back(rtsp_response(200, must_plist(info_plist((int64_t)mac_features))));
	mac_tv.replies.push_back(rtsp_response(403, {}));
	mac_tv.replies.push_back(rtsp_response(200, {}));
	airplay::HandshakeInput mac_pin = base_input();
	mac_pin.receiver.features = mac_features;
	mac_pin.receiver.status_flags = 0x204;
	bool mac_prompted = false;
	mac_pin.request_pin = [&]() {
		mac_prompted = mac_tv.written.find("X-Apple-HKP: 5") != std::string::npos &&
			       mac_tv.written.find("X-Apple-HKP: 3") != std::string::npos;
		return std::string();
	};
	airplay::HandshakeResult mac_result = airplay::handshake(mac_tv, mac_pin);
	CHECK(!mac_result.ok);
	CHECK(mac_prompted);
	CHECK(mac_result.error.find("Enter the pairing code") != std::string::npos);
	CHECK(mac_result.error.find("did not show") == std::string::npos);
	CHECK(mac_tv.written.find("pair-setup") == std::string::npos);
	CHECK(mac_result.requests.size() == 3);

	ScriptedTransport flag_only;
	flag_only.replies.push_back(rtsp_response(200, must_plist(info_plist(transient))));
	flag_only.replies.push_back(rtsp_response(404, {}));
	airplay::HandshakeInput flag_input = base_input();
	flag_input.receiver.features = (uint64_t)transient;
	flag_input.receiver.status_flags = 0x40;
	bool flag_prompted = false;
	flag_input.request_pin = [&]() {
		flag_prompted = true;
		return std::string("1357");
	};
	airplay::HandshakeResult flag_result = airplay::handshake(flag_only, flag_input);
	CHECK(!flag_result.ok);
	CHECK(!flag_prompted);
	CHECK(flag_result.error.find("did not show") == std::string::npos);
	CHECK(flag_result.error.find("1357") == std::string::npos);
	CHECK(flag_only.written.find("/pair-setup") != std::string::npos);
	CHECK(flag_only.written.find("X-Apple-HKP: 4") != std::string::npos);
	CHECK(flag_only.written.find(transient_flag) != std::string::npos);

	struct Cross : airplay::ByteTransport {
		std::vector<uint8_t> *inbox = nullptr;
		std::vector<uint8_t> *outbox = nullptr;
		size_t offset = 0;
		Cross(std::vector<uint8_t> *in, std::vector<uint8_t> *out) : inbox(in), outbox(out) {}
		bool write_all(const uint8_t *data, size_t size) override
		{
			outbox->insert(outbox->end(), data, data + size);
			return true;
		}
		int read_some(uint8_t *data, size_t capacity, int) override
		{
			if (offset >= inbox->size())
				return 0;
			size_t n = inbox->size() - offset;
			if (n > capacity)
				n = capacity;
			std::memcpy(data, inbox->data() + offset, n);
			offset += n;
			return (int)n;
		}
	};
	std::vector<uint8_t> client_to_server;
	std::vector<uint8_t> server_to_client;
	Cross client_io{&server_to_client, &client_to_server};
	Cross server_io{&client_to_server, &server_to_client};
	std::vector<uint8_t> secret(64, 9);
	airplay::SealedControl client(client_io, secret, false);
	airplay::SealedControl server(server_io, secret, true);
	std::vector<uint8_t> message(2000, 0x5a);
	CHECK(client.write_all(message.data(), message.size()));
	std::vector<uint8_t> opened(message.size());
	size_t got = 0;
	while (got < opened.size()) {
		int n = server.read_some(opened.data() + got, opened.size() - got, 0);
		CHECK(n > 0);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	CHECK(got == message.size());
	CHECK(opened == message);
	airplay::fairplay_install_hooks(nullptr);
}

} /* namespace */

void test_pairing_store()
{
	airplay::PairingIdentity first;
	CHECK(airplay::pairing_identity_persistent(first));
	CHECK(first.id.size() == 36);
	airplay::PairingIdentity again;
	CHECK(airplay::pairing_identity_persistent(again));
	CHECK(again.id == first.id);
	CHECK(std::memcmp(again.private_key, first.private_key, 64) == 0);
	CHECK(std::memcmp(again.public_key, first.private_key + 32, 32) == 0);
	const char *device = "AA:BB:CC:DD:EE:FF";
	CHECK(!airplay::pairing_device_remembered(device));
	airplay::pairing_device_remember(device);
	CHECK(airplay::pairing_device_remembered("aabbccddeeff"));
	airplay::pairing_device_forget(device);
	CHECK(!airplay::pairing_device_remembered(device));
}

void test_feedback()
{
	ScriptedTransport io;
	io.replies.push_back(rtsp_response(200, {}));
	int cseq = 7;
	std::string error;
	CHECK(airplay::post_feedback(io, cseq, error));
	CHECK(error.empty());
	CHECK(cseq == 8);
	CHECK(io.written.find("POST /feedback RTSP/1.0\r\n") == 0);
	CHECK(io.written.find("CSeq: 7\r\n") != std::string::npos);
	CHECK(io.written.find("Content-Length: 0\r\n") != std::string::npos);
	io.written.clear();
	io.replies.push_back(rtsp_response(500, {}));
	io.index = 1;
	CHECK(!airplay::post_feedback(io, cseq, error));
	CHECK(error.find("500") != std::string::npos);

	const char *request = "POST /feedback RTSP/1.0\r\nCSeq: 4\r\nContent-Length: 0\r\n\r\n";
	std::vector<uint8_t> to_sender(request, request + std::strlen(request));
	std::vector<uint8_t> to_receiver;
	struct Pipe : airplay::ByteTransport {
		std::vector<uint8_t> *inbox = nullptr;
		std::vector<uint8_t> *outbox = nullptr;
		size_t offset = 0;
		Pipe(std::vector<uint8_t> *in, std::vector<uint8_t> *out) : inbox(in), outbox(out) {}
		bool write_all(const uint8_t *data, size_t size) override
		{
			outbox->insert(outbox->end(), data, data + size);
			return true;
		}
		int read_some(uint8_t *data, size_t capacity, int) override
		{
			if (offset >= inbox->size())
				return 0;
			size_t n = inbox->size() - offset;
			if (n > capacity)
				n = capacity;
			std::memcpy(data, inbox->data() + offset, n);
			offset += n;
			return (int)n;
		}
	};
	Pipe sender_io{&to_sender, &to_receiver};
	std::vector<uint8_t> pending;
	error.clear();
	CHECK(airplay::answer_event(sender_io, pending, nullptr, error));
	CHECK(error.empty());
	std::string reply(to_receiver.begin(), to_receiver.end());
	CHECK(reply.find("RTSP/1.0 200 OK\r\n") == 0);
	CHECK(reply.find("CSeq: 4\r\n") != std::string::npos);

	uint8_t read_key[32];
	uint8_t write_key[32];
	for (int i = 0; i < 32; i++) {
		read_key[i] = (uint8_t)(i + 1);
		write_key[i] = (uint8_t)(i + 40);
	}
	std::vector<uint8_t> receiver_out;
	std::vector<uint8_t> sender_out;
	Pipe receiver_side{&sender_out, &receiver_out};
	Pipe sender_side{&receiver_out, &sender_out};
	airplay::SealedControl receiver(receiver_side, read_key, write_key);
	CHECK(receiver.write_all(reinterpret_cast<const uint8_t *>(request), std::strlen(request)));
	airplay::SealedControl sender(sender_side, write_key, read_key);
	pending.clear();
	CHECK(airplay::answer_event(sender, pending, nullptr, error));
	std::vector<uint8_t> opened;
	uint8_t chunk[256];
	for (;;) {
		int n = receiver.read_some(chunk, sizeof chunk, 0);
		if (n <= 0)
			break;
		opened.insert(opened.end(), chunk, chunk + n);
	}
	std::string sealed_reply(opened.begin(), opened.end());
	CHECK(sealed_reply.find("RTSP/1.0 200 OK\r\n") == 0);
	CHECK(sealed_reply.find("CSeq: 4\r\n") != std::string::npos);
}

void test_bitstream()
{
	/* Parameter sets and descriptions from VideoToolbox, 1280x720. */
	airplay::ParameterSets hevc;
	hevc.vps = bytes_of("40010c01ffff016000000300b0000003000003005d170240");
	hevc.sps = bytes_of("420101016000000300b0000003000003005da00280802d16205ee45914bff2e7f13fa2");
	hevc.pps = bytes_of("4401c072f05324");
	const std::vector<uint8_t> vt_hvcc = bytes_of("010160000000b000000000005df000fcfdf8f800000b03a00001001840010c01"
						      "ffff016000000300b0000003000003005d170240a10001002342010101600000"
						      "0300b0000003000003005da00280802d16205ee45914bff2e7f13fa2a2000100"
						      "074401c072f05324");
	std::vector<uint8_t> hvcc;
	CHECK(airplay::build_hvcc(hevc, hvcc));
	CHECK(hvcc == vt_hvcc);
	const std::vector<uint8_t> vt_hevc_desc =
		bytes_of("000000ca68766331000000000000000100000000000000000000020000000200"
			 "050002d000480000004800000000000000010448455643000000000000000000"
			 "0000000000000000000000000000000000000018ffff00000070687663430101"
			 "60000000b000000000005df000fcfdf8f800000b03a00001001840010c01ffff"
			 "016000000300b0000003000003005d170240a100010023420101016000000300"
			 "b0000003000003005da00280802d16205ee45914bff2e7f13fa2a20001000744"
			 "01c072f0532400000000");
	CHECK(airplay::image_description(airplay::Codec::Hevc, 1280, 720, vt_hvcc) == vt_hevc_desc);

	airplay::ParameterSets avc;
	avc.sps = bytes_of("2764001fac56805005b9");
	avc.pps = bytes_of("28ee3cb0");
	std::vector<uint8_t> avcc;
	CHECK(airplay::build_avcc(avc, avcc));
	CHECK(avcc == bytes_of("0164001fffe1000a2764001fac56805005b901000428ee3cb0fdf8f800"));
	const std::vector<uint8_t> vt_avc_desc =
		bytes_of("0000007f61766331000000000000000100000000000000000000020000000200"
			 "050002d0004800000048000000000000000105482e3236340000000000000000"
			 "0000000000000000000000000000000000000018ffff00000025617663430164"
			 "001fffe1000a2764001fac56805005b901000428ee3cb0fdf8f80000000000");
	CHECK(airplay::image_description(airplay::Codec::H264, 1280, 720, avcc) == vt_avc_desc);

	std::vector<uint8_t> annexb = bytes_of("00000001");
	annexb.insert(annexb.end(), hevc.vps.begin(), hevc.vps.end());
	const std::vector<uint8_t> rest = bytes_of("00000001");
	annexb.insert(annexb.end(), rest.begin(), rest.end());
	annexb.insert(annexb.end(), hevc.sps.begin(), hevc.sps.end());
	const std::vector<uint8_t> tail = bytes_of("000001"
						   "4401c072f05324"
						   "00000001"
						   "460150"
						   "00000001"
						   "2601af0400");
	annexb.insert(annexb.end(), tail.begin(), tail.end());
	airplay::ParameterSets found;
	std::vector<uint8_t> frame;
	CHECK(airplay::annexb_to_length_prefixed(airplay::Codec::Hevc, annexb.data(), annexb.size(), frame, found));
	CHECK(frame == bytes_of("000000042601af04"));
	CHECK(found.vps == hevc.vps);
	CHECK(found.sps == hevc.sps);
	CHECK(found.pps == hevc.pps);
}

void test_mdns()
{
	std::vector<uint8_t> query = airplay::mdns_query(0x1234, {{"_airplay._tcp.local.", 12}});
	CHECK(hex_of(query.data(), query.size()) ==
	      "123400000001000000000000085f616972706c6179045f746370056c6f63616c00000c8001");

	/* A response with every name after the first compressed. */
	std::vector<uint8_t> p = bytes_of("123484000000000400000000");
	auto add = [&](const char *hex) {
		std::vector<uint8_t> more = bytes_of(hex);
		p.insert(p.end(), more.begin(), more.end());
	};
	/* PTR _airplay._tcp.local. -> "Office TV" + pointer to offset 12. */
	add("085f616972706c6179045f746370056c6f63616c00"
	    "000c0001"
	    "00001194"
	    "000c"
	    "094f6666696365205456c00c");
	/* SRV for the instance (pointer to the PTR rdata at offset 0x2b), port 7000, target tv.local. */
	add("c02b"
	    "00218001"
	    "00000078"
	    "000b"
	    "00000000"
	    "1b58"
	    "027476c01a");
	/* TXT model=7203X and deviceid. */
	add("c02b"
	    "00108001"
	    "00001194"
	    "0021"
	    "0b6d6f64656c3d3732303358"
	    "146465766963656964"
	    "3d41323a41413a33463a4442");
	/* A for tv.local. (pointer to the SRV target). */
	add("c049"
	    "00018001"
	    "00000078"
	    "0004"
	    "0a2f162f");
	std::vector<airplay::MdnsRecord> records = airplay::mdns_parse(p.data(), p.size());
	CHECK(records.size() == 4);
	if (records.size() != 4)
		return;
	CHECK(records[0].type == 12 && records[0].name == "_airplay._tcp.local.");
	CHECK(records[0].target == "office tv._airplay._tcp.local." && records[0].instance == "Office TV");
	CHECK(records[1].type == 33 && records[1].name == "office tv._airplay._tcp.local.");
	CHECK(records[1].port == 7000 && records[1].target == "tv.local.");
	CHECK(records[2].type == 16 && records[2].txt.size() == 0x21);
	CHECK(records[3].type == 1 && records[3].name == "tv.local." && records[3].address == "10.47.22.47");

	std::vector<uint8_t> loop = bytes_of("123484000000000100000000c00c000c00010000000000020000");
	CHECK(airplay::mdns_parse(loop.data(), loop.size()).empty());
}

void test_fairplay_sender()
{
	uint8_t m1[fairplay_sap::kM1Size];
	fairplay_sap::build_m1(m1);
	CHECK(hex_of(m1, sizeof m1) == "46504c590301010000000004020003bb");
	for (const FairPlayVector &v : kFairPlayVectors) {
		std::vector<uint8_t> m2 = bytes_of(v.m2), entropy = bytes_of(v.sap_entropy), raw = bytes_of(v.raw_key),
				     mask = bytes_of(v.mask);
		uint8_t m3[fairplay_sap::kM3Size];
		const char *error = nullptr;
		CHECK(fairplay_sap::build_m3(m2.data(), m2.size(), entropy.data(), m3, &error));
		CHECK(hex_of(m3, sizeof m3) == v.m3);
		uint8_t aes_key[16], mac_key[20];
		CHECK(fairplay_sap::wrap_keys(m2.data(), m2.size(), m3, sizeof m3, aes_key, mac_key, &error));
		std::vector<uint8_t> ekey;
		airplay::fairplay_ekey(aes_key, mac_key, raw.data(), mask.data(), ekey);
		CHECK(hex_of(ekey.data(), ekey.size()) == v.ekey);
	}
	std::vector<uint8_t> m2 = bytes_of(kFairPlayVectors[0].m2), entropy = bytes_of(kFairPlayVectors[0].sap_entropy);
	uint8_t m3[fairplay_sap::kM3Size];
	const char *error = nullptr;
	m2[13] = 1;
	CHECK(!fairplay_sap::build_m3(m2.data(), m2.size(), entropy.data(), m3, &error));
	CHECK(error && std::strstr(error, "mode") != nullptr);
	CHECK(!fairplay_sap::build_m3(m2.data(), 141, entropy.data(), m3, &error));
}

int main()
{
	test_mdns();
	test_fairplay_sender();
	test_bitstream();
	test_feedback();
	test_pairing_store();
	test_features();
	test_choose_encode();
	test_mirror_and_ntp();
	test_rtsp_plist_rtp();
	test_audio_setup();
	test_crypto();
	test_handshake();
	test_fairplay_and_pairing();
	if (g_failures) {
		std::fprintf(stderr, "%d checks failed\n", g_failures);
		return 1;
	}
	std::printf("protocol tests passed\n");
	return 0;
}
