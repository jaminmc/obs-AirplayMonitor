/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/handshake.hpp"

#include "airplay/aead.hpp"
#include "airplay/net.hpp"
#include "airplay/portable.hpp"
#include "airplay/crypto.hpp"
#include "airplay/fairplay.hpp"
#include "airplay/pairing.hpp"
#include "airplay/plist.hpp"
#include "airplay/rtsp.hpp"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace airplay {
namespace {

const char *kCodeRequired = "Enter the pairing code shown on the TV, then connect again.";
const char *kCodeRejected = "The pairing code was rejected.";
const char *kFairPlayFailed = "FairPlay setup failed.";
const char *kFairPlayMissing = "This receiver does not accept FairPlay setup.";

std::string uuid_string()
{
	uint8_t bytes[16];
	random_bytes(bytes, sizeof bytes);
	bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
	bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
	char text[40];
	std::snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
		      bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
	return text;
}

Plist name_array(std::initializer_list<const char *> names)
{
	std::vector<Plist> items;
	for (const char *name : names) {
		std::map<std::string, Plist> entry;
		entry.emplace("name", Plist::string(name));
		items.push_back(Plist::dictionary(std::move(entry)));
	}
	return Plist::arr(std::move(items));
}

struct Reader {
	ByteTransport *transport = nullptr;
	std::vector<uint8_t> buffer;
	int timeout_ms = 20000;

	bool next(RtspMessage &message, std::string &error)
	{
		for (int spins = 0; spins < 40; spins++) {
			size_t consumed = 0;
			if (parse_rtsp_message(buffer.data(), buffer.size(), message, consumed)) {
				buffer.erase(buffer.begin(), buffer.begin() + (std::ptrdiff_t)consumed);
				return true;
			}
			uint8_t chunk[4096];
			int count = transport->read_some(chunk, sizeof chunk, timeout_ms);
			if (count < 0) {
				error = "The receiver closed the connection";
				return false;
			}
			if (count == 0) {
				error = "Timed out waiting for the receiver";
				return false;
			}
			buffer.insert(buffer.end(), chunk, chunk + count);
		}
		error = "The receiver sent a response that could not be read";
		return false;
	}
};

bool send_request(ByteTransport &transport, const std::string &method, const std::string &uri, int cseq,
		  const std::vector<uint8_t> *body, const char *content_type,
		  const std::map<std::string, std::string> *extra, std::string &error)
{
	std::map<std::string, std::string> headers;
	headers["X-Apple-ProtocolVersion"] = "1";
	if (extra) {
		for (const auto &header : *extra)
			headers[header.first] = header.second;
	}
	if (content_type) {
		headers["Content-Type"] = content_type;
		headers["Content-Length"] = std::to_string(body ? body->size() : 0);
	}
	std::string request =
		make_rtsp_request(method, uri, cseq, headers, body ? body->data() : nullptr, body ? body->size() : 0);
	if (!transport.write_all(reinterpret_cast<const uint8_t *>(request.data()), request.size())) {
		error = "Could not write to the receiver";
		return false;
	}
	return true;
}

std::string uri_for(const ReceiverInfo &receiver, const std::string &path)
{
	std::ostringstream uri;
	uri << "rtsp://" << receiver.address << ':' << receiver.port << path;
	return uri.str();
}

void note(HandshakeResult &result, const std::string &method, const std::string &path)
{
	result.requests.push_back(method + " " + path);
}

void observe_setup_ports(const Plist &body, HandshakeResult &result, const HandshakeInput &input, const char *label)
{
	if (body.type == Plist::Type::Dict) {
		result.setup_detail += label;
		for (const auto &entry : body.dict)
			result.setup_detail += " " + entry.first;
		result.setup_detail += "\n";
	}
	int event = (int)body.get_int("eventPort");
	if (event > 0)
		result.event_port = event;
	int timing = (int)body.get_int("timingPort");
	if (timing > 0 && result.receiver_timing_port <= 0) {
		result.receiver_timing_port = timing;
		if (input.on_receiver_timing)
			input.on_receiver_timing(timing);
	}
}

std::string sender_mac(const std::string &id)
{
	std::string hex;
	for (unsigned char c : id) {
		if (std::isxdigit(c))
			hex.push_back((char)std::tolower(c));
	}
	if (hex.size() < 12)
		return "02:00:00:00:00:01";
	std::string mac = "02";
	for (int i = 2; i < 12; i += 2) {
		mac.push_back(':');
		mac.append(hex, (size_t)i, 2);
	}
	return mac;
}

void read_displays(const Plist &info, ReceiverInfo &receiver)
{
	const Plist *displays = info.find("displays");
	if (!displays || displays->type != Plist::Type::Array)
		return;
	receiver.displays.clear();
	for (const Plist &item : displays->array) {
		DisplayMode mode;
		mode.width = (uint32_t)item.get_int("widthPixels");
		if (!mode.width)
			mode.width = (uint32_t)item.get_int("width");
		mode.height = (uint32_t)item.get_int("heightPixels");
		if (!mode.height)
			mode.height = (uint32_t)item.get_int("height");
		mode.max_fps = (uint32_t)item.get_int("maxFPS");
		if (!mode.max_fps) {
			double refresh = item.get_real("refreshRate");
			if (refresh > 1)
				mode.max_fps = (uint32_t)refresh;
		}
		mode.features = (uint64_t)item.get_int("features");
		mode.hevc = item.get_bool("hevc");
		receiver.displays.push_back(mode);
	}
}

std::string response_hint(const RtspMessage &message)
{
	std::ostringstream hint;
	if (!message.reason.empty())
		hint << " reason=" << message.reason;
	for (const auto &header : message.headers) {
		if (header.second.size() <= 120)
			hint << ' ' << header.first << '=' << header.second;
	}
	std::string type = header_get(message, "content-type");
	if (!type.empty() && message.headers.count("content-type") == 0 && message.headers.count("Content-Type") == 0)
		hint << " type=" << type;
	if (message.body.empty())
		return hint.str();
	Plist body;
	std::string decode_error;
	if (decode_plist(message.body.data(), message.body.size(), body, decode_error) &&
	    body.type == Plist::Type::Dict) {
		hint << " keys";
		for (const auto &entry : body.dict) {
			hint << ' ' << entry.first;
			if (entry.second.type == Plist::Type::String && entry.second.s.size() <= 80)
				hint << '=' << entry.second.s;
			else if (entry.second.type == Plist::Type::Int)
				hint << '=' << entry.second.i;
			else if (entry.second.type == Plist::Type::Bool)
				hint << '=' << (entry.second.b ? "true" : "false");
		}
		return hint.str();
	}
	hint << " body_bytes=" << message.body.size();
	return hint.str();
}

int port_from(const Plist &plist, const char *key)
{
	if (plist.find(key))
		return (int)plist.get_int(key);
	const Plist *streams = plist.find("streams");
	if (!streams || streams->array.empty())
		return 0;
	return (int)streams->array[0].get_int(key);
}

int positive_port(const Plist &plist, const char *key)
{
	const Plist *value = plist.find(key);
	if (!value)
		return 0;
	int port = (int)value->i;
	if (port > 0 && port <= 65535)
		return port;
	return 0;
}

int find_port(const Plist &plist, const char *key);

/* A Mac answers type 96 with the RTP port under the stream connection, not dataPort. */
void audio_ports_from(const Plist &body, int &data_port, int &control_port)
{
	data_port = 0;
	control_port = 0;
	const Plist *stream = nullptr;
	if (const Plist *streams = body.find("streams")) {
		for (const Plist &item : streams->array) {
			if (!stream || item.get_int("type") == 96)
				stream = &item;
			if (item.get_int("type") == 96)
				break;
		}
	}
	const Plist &node = stream ? *stream : body;
	data_port = positive_port(node, "dataPort");
	control_port = positive_port(node, "controlPort");
	if (const Plist *connections = node.find("streamConnections")) {
		if (data_port <= 0) {
			if (const Plist *rtp = connections->find("streamConnectionTypeRTP")) {
				data_port = positive_port(*rtp, "streamConnectionKeyPort");
				if (data_port <= 0)
					data_port = positive_port(*rtp, "streamConnectionKeyDataPort");
			}
		}
		if (control_port <= 0) {
			if (const Plist *rtcp = connections->find("streamConnectionTypeRTCP"))
				control_port = positive_port(*rtcp, "streamConnectionKeyPort");
		}
	}
	if (data_port <= 0)
		data_port = find_port(body, "dataPort");
}

int find_port(const Plist &plist, const char *key)
{
	if (const Plist *direct = plist.find(key)) {
		int port = (int)direct->i;
		if (port > 0)
			return port;
	}
	if (plist.type == Plist::Type::Dict) {
		for (const auto &entry : plist.dict) {
			int port = find_port(entry.second, key);
			if (port > 0)
				return port;
		}
	} else if (plist.type == Plist::Type::Array) {
		for (const Plist &item : plist.array) {
			int port = find_port(item, key);
			if (port > 0)
				return port;
		}
	}
	return 0;
}

bool auth_failure(int status)
{
	return status == 401 || status == 403 || status == 470;
}

std::string auth_error(int status)
{
	return "The receiver refused the connection (RTSP " + std::to_string(status) + ").";
}

bool codec_rejected(int status)
{
	return status == 406 || status == 415 || status == 455 || status == 501;
}

bool tlv_error_is(const std::vector<uint8_t> &body, uint8_t code)
{
	std::vector<uint8_t> value;
	if (!tlv_get(body, (uint8_t)TlvType::Error, value) || value.empty())
		return false;
	return value[0] == code;
}

bool post_octet(HandshakeResult &result, ByteTransport &io, Reader &reader, const ReceiverInfo &receiver,
		const char *path, int &cseq, const std::vector<uint8_t> &body,
		const std::map<std::string, std::string> &extra, RtspMessage &response, std::string &error)
{
	note(result, "POST", path);
	if (!send_request(io, "POST", uri_for(receiver, path), cseq++, &body, "application/octet-stream", &extra,
			  error))
		return false;
	return reader.next(response, error);
}

struct SecretWipe {
	std::string *text = nullptr;
	~SecretWipe()
	{
		if (!text || text->empty())
			return;
		volatile char *bytes = &(*text)[0];
		for (size_t i = 0; i < text->size(); i++)
			bytes[i] = 0;
		text->clear();
	}
};

bool tlv_intact(const std::vector<uint8_t> &message)
{
	size_t i = 0;
	while (i + 2 <= message.size()) {
		size_t n = message[i + 1];
		if (i + 2 + n > message.size())
			return false;
		i += 2 + n;
	}
	return i == message.size();
}

/* True when an error TLV is present. `authentication` is set for code 0x02. */
bool tlv_failed(const std::vector<uint8_t> &body, bool &authentication)
{
	authentication = false;
	std::vector<uint8_t> value;
	if (!tlv_get(body, (uint8_t)TlvType::Error, value))
		return false;
	authentication = !value.empty() && value[0] == kTlvErrorAuthentication;
	return true;
}

std::string sender_name()
{
	char host[256] = {};
	net::startup();
	if (gethostname(host, sizeof host - 1) == 0 && host[0] != '\0') {
		std::string name = host;
		size_t dot = name.find('.');
		if (dot != std::string::npos)
			name.resize(dot);
		if (!name.empty())
			return name;
	}
	return "AirPlay Monitor";
}

std::map<std::string, std::string> pin_headers(int hkp, const std::string &client_id, bool lengths, bool pd)
{
	std::map<std::string, std::string> headers;
	headers.emplace("X-Apple-HKP", std::to_string(hkp));
	headers.emplace("X-Apple-Client-Name", sender_name());
	headers.emplace("X-Apple-Client-ID", client_id);
	if (lengths)
		headers.emplace("X-Apple-SupportedPINLengths", "4");
	if (pd)
		headers.emplace("X-Apple-PD", "1");
	return headers;
}

enum class VerifyOutcome { Ok, Rejected, Closed };

VerifyOutcome pair_verify_exchange(HandshakeResult &result, ByteTransport &io, Reader &reader,
				   const ReceiverInfo &receiver, int &cseq, int hkp, const PairingIdentity &identity,
				   std::vector<uint8_t> &channel, RtspMessage &response, std::string &error)
{
	VerifyEphemeral ephemeral;
	std::vector<uint8_t> v1;
	/* This TV rejects a method-7 verify for a pairing created with method 3.
	   The dump's method 6 is a Mac-created pairing, not this setup. */
	if (!pair_verify_m1(ephemeral, v1)) {
		result.error = "Pairing failed.";
		return VerifyOutcome::Rejected;
	}
	std::map<std::string, std::string> verify_headers = pin_headers(hkp, identity.id, false, true);
	if (!post_octet(result, io, reader, receiver, "/pair-verify", cseq, v1, verify_headers, response, error)) {
		result.error = error;
		return VerifyOutcome::Closed;
	}
	bool authentication = false;
	if (response.status != 200 || !tlv_intact(response.body) || tlv_failed(response.body, authentication))
		return VerifyOutcome::Rejected;
	std::vector<uint8_t> verify_pub;
	if (!tlv_get(response.body, (uint8_t)TlvType::PublicKey, verify_pub) || verify_pub.size() < 32)
		return VerifyOutcome::Rejected;
	std::vector<uint8_t> verify_blob;
	const std::vector<uint8_t> *blob = nullptr;
	if (tlv_get(response.body, (uint8_t)TlvType::EncryptedData, verify_blob))
		blob = &verify_blob;
	std::vector<uint8_t> v3;
	if (!pair_verify_finish(identity, ephemeral, verify_pub, blob, channel, v3))
		return VerifyOutcome::Rejected;
	if (!post_octet(result, io, reader, receiver, "/pair-verify", cseq, v3, verify_headers, response, error)) {
		secure_clear(channel);
		result.error = error;
		return VerifyOutcome::Closed;
	}
	if (response.status != 200) {
		secure_clear(channel);
		return VerifyOutcome::Rejected;
	}
	if ((!response.body.empty() && !tlv_intact(response.body)) || tlv_failed(response.body, authentication)) {
		secure_clear(channel);
		return VerifyOutcome::Rejected;
	}
	return VerifyOutcome::Ok;
}

bool finish_pin_setup(HandshakeResult &result, ByteTransport &io, Reader &reader, const ReceiverInfo &receiver,
		      int &cseq, int hkp, const PairingIdentity &identity, const std::string &password,
		      std::vector<uint8_t> &channel, RtspMessage &response, std::string &error,
		      const std::function<bool()> &before_verify)
{
	std::map<std::string, std::string> headers = pin_headers(hkp, identity.id, false, false);
	std::vector<uint8_t> m1 = pair_setup_m1_pin();
	if (!post_octet(result, io, reader, receiver, "/pair-setup", cseq, m1, headers, response, error)) {
		result.error = error;
		return false;
	}
	if (response.status != 200) {
		result.error = "Pairing failed (RTSP " + std::to_string(response.status) + ").";
		return false;
	}
	bool authentication = false;
	if (!tlv_intact(response.body) || tlv_failed(response.body, authentication)) {
		result.error = authentication ? "The TV rejected pairing setup." : "Pairing failed.";
		return false;
	}
	std::vector<uint8_t> salt;
	std::vector<uint8_t> server_pub;
	if (!tlv_get(response.body, (uint8_t)TlvType::Salt, salt) ||
	    !tlv_get(response.body, (uint8_t)TlvType::PublicKey, server_pub)) {
		result.error = "Pairing failed.";
		return false;
	}
	SrpClient client;
	std::vector<uint8_t> client_proof;
	if (!client.start(password, error) || !client.proof(salt, server_pub, client_proof, error)) {
		result.error = "Pairing failed.";
		return false;
	}
	TlvItem state_item;
	state_item.type = (uint8_t)TlvType::State;
	state_item.value.push_back(3);
	TlvItem public_item;
	public_item.type = (uint8_t)TlvType::PublicKey;
	public_item.value = client.public_key();
	TlvItem proof_item;
	proof_item.type = (uint8_t)TlvType::Proof;
	proof_item.value = std::move(client_proof);
	std::vector<uint8_t> m3 = tlv_encode({state_item, public_item, proof_item});
	if (!post_octet(result, io, reader, receiver, "/pair-setup", cseq, m3, headers, response, error)) {
		result.error = error;
		return false;
	}
	if (response.status != 200) {
		result.error = "Pairing failed (RTSP " + std::to_string(response.status) + ").";
		return false;
	}
	if (!tlv_intact(response.body) || tlv_failed(response.body, authentication)) {
		result.error = authentication ? kCodeRejected : "Pairing failed.";
		return false;
	}
	std::vector<uint8_t> server_proof;
	if (!tlv_get(response.body, (uint8_t)TlvType::Proof, server_proof) || !client.verify_server(server_proof)) {
		result.error = "Pairing failed.";
		return false;
	}

	std::vector<uint8_t> session = client.session_key();
	struct SessionWipe {
		std::vector<uint8_t> &key;
		~SessionWipe() { secure_clear(key); }
	} session_wipe{session};

	std::vector<uint8_t> m5;
	if (identity.id.empty() || !pair_setup_m5(identity, session, hkp == 5, m5)) {
		result.error = "Pairing failed.";
		return false;
	}
	if (!post_octet(result, io, reader, receiver, "/pair-setup", cseq, m5, headers, response, error)) {
		result.error = error;
		return false;
	}
	if (response.status != 200) {
		result.error = "Pairing failed (RTSP " + std::to_string(response.status) + ").";
		return false;
	}
	if (!response.body.empty() && !tlv_intact(response.body)) {
		result.error = "Pairing failed.";
		return false;
	}
	if (tlv_failed(response.body, authentication)) {
		result.error = authentication ? "The TV rejected the pairing identity." : "Pairing failed.";
		return false;
	}
	if (before_verify && !before_verify())
		return false;
	VerifyOutcome verified =
		pair_verify_exchange(result, io, reader, receiver, cseq, hkp, identity, channel, response, error);
	if (verified == VerifyOutcome::Ok)
		return true;
	if (result.error.empty())
		result.error = "Pairing failed.";
	return false;
}

HandshakeResult run_once(ByteTransport &transport, const HandshakeInput &input, bool allow_hevc, bool &retry_h264,
			 std::unique_ptr<SealedControl> *retain_seal)
{
	HandshakeResult result;
	retry_h264 = false;
	ReceiverInfo receiver = input.receiver;
	int cseq = 1;
	std::string error;
	ByteTransport *io = &transport;
	Reader reader{io, {}, input.response_timeout_ms > 0 ? input.response_timeout_ms : 20000};
	std::unique_ptr<SealedControl> sealed;
	std::vector<uint8_t> srp_key;
	struct KeyWipe {
		std::vector<uint8_t> &key;
		~KeyWipe() { secure_clear(key); }
	} wipe_srp{srp_key};
	std::string info_uri = uri_for(receiver, "/info");

	std::map<std::string, Plist> qualifier;
	qualifier.emplace("qualifier", Plist::arr({Plist::string("txtAirPlay")}));
	std::vector<uint8_t> info_body;
	if (!encode_bplist(Plist::dictionary(qualifier), info_body, error)) {
		result.error = error;
		return result;
	}
	note(result, "GET", "/info");
	if (!send_request(*io, "GET", info_uri, cseq++, &info_body, "application/x-apple-binary-plist", nullptr,
			  error)) {
		result.error = error;
		return result;
	}
	RtspMessage response;
	if (!reader.next(response, error)) {
		result.error = error;
		return result;
	}
	if (auth_failure(response.status)) {
		result.error = auth_error(response.status);
		return result;
	}
	if (response.status != 200) {
		result.error = "GET /info failed (RTSP " + std::to_string(response.status) + ")";
		return result;
	}

	if (!response.body.empty()) {
		Plist info;
		std::string plist_error;
		if (decode_plist(response.body.data(), response.body.size(), info, plist_error)) {
			int64_t features = info.get_int("features", -1);
			if (features >= 0)
				receiver.features |= (uint64_t)features;
			if (info.find("statusFlags"))
				receiver.status_flags = (uint32_t)info.get_int("statusFlags");
			if (receiver.model.empty())
				receiver.model = info.get_string("model");
			if (receiver.device_id.empty())
				receiver.device_id = info.get_string("deviceID");
			read_displays(info, receiver);
		}
	}

	result.fairplay_advertised = feature_bit(receiver.features, (int)FeatureBit::FairPlayAuth) ||
				     feature_bit(receiver.features, (int)FeatureBit::FairPlayAesGcm) ||
				     feature_bit(receiver.features, (int)FeatureBit::UnifiedPairing);
	DisplayMode display = best_display(receiver.displays);
	bool hevc = allow_hevc && advertise_hevc(receiver.features, receiver.displays);
	result.max_width = display.width;
	result.max_height = display.height;
	result.max_fps = display.max_fps;
	result.config = choose_encode(input.src_width, input.src_height, input.src_fps, display.width, display.height,
				      display.max_fps, hevc, input.bitrate_override_bps);

	uint8_t raw_key[16];
	uint8_t raw_iv[16];
	random_bytes(raw_key, sizeof raw_key);
	random_bytes(raw_iv, sizeof raw_iv);
	std::vector<uint8_t> ekey;
	bool have_ekey = false;
	bool fairplay_absent = false;
	bool fairplay_bits = feature_bit(receiver.features, (int)FeatureBit::FairPlayAuth) ||
			     feature_bit(receiver.features, (int)FeatureBit::FairPlayAesGcm);
	auto exchange_fairplay = [&](bool allow_missing) -> bool {
		FairPlaySap sap;
		std::vector<uint8_t> fp_m1;
		if (!sap.m1(fp_m1, error)) {
			result.error = error.empty() ? kFairPlayFailed : error;
			return false;
		}
		std::map<std::string, std::string> et;
		et.emplace("X-Apple-ET", "32");
		if (!post_octet(result, *io, reader, receiver, "/fp-setup", cseq, fp_m1, et, response, error)) {
			result.error = error;
			return false;
		}
		if (response.status == 404) {
			if (allow_missing) {
				fairplay_absent = true;
				return true;
			}
			result.error = kFairPlayMissing;
			return false;
		}
		if (response.status != 200 || response.body.empty()) {
			result.error = "FairPlay setup failed (RTSP " + std::to_string(response.status) + ")" +
				       response_hint(response);
			return false;
		}
		std::vector<uint8_t> m2 = std::move(response.body);
		std::vector<uint8_t> fp_m3;
		if (!sap.m3(m2, fp_m3, error)) {
			result.error = kFairPlayFailed;
			return false;
		}
		if (!post_octet(result, *io, reader, receiver, "/fp-setup", cseq, fp_m3, et, response, error)) {
			result.error = error;
			return false;
		}
		if (response.status == 404) {
			result.error = kFairPlayMissing;
			return false;
		}
		if (response.status != 200 || response.body.size() < 16 ||
		    std::memcmp(response.body.data(), "FPLY", 4) != 0) {
			result.error = "FairPlay setup failed (RTSP " + std::to_string(response.status) + ")" +
				       response_hint(response);
			return false;
		}
		if (!sap.wrap(m2, fp_m3, raw_key, ekey, error) || ekey.size() != 72) {
			result.error = kFairPlayFailed;
			return false;
		}
		have_ekey = true;
		return true;
	};

	bool pair = requires_pairing(receiver.features, receiver.status_flags);
	bool saved_pairing = false;
	if (pair) {
		std::string password;
		SecretWipe wipe_password{&password};
		if (requires_one_time_code(receiver.status_flags)) {
			PairingIdentity identity;
			if (!pairing_identity_persistent(identity)) {
				result.error = "Could not store the pairing identity.";
				return result;
			}
			std::string device_id = receiver.device_id.empty() ? receiver.address : receiver.device_id;
			int hkp = pin_hkp(receiver.features);
			bool verified = false;
			if (pairing_device_remembered(device_id)) {
				VerifyOutcome outcome = pair_verify_exchange(result, *io, reader, receiver, cseq, hkp,
									     identity, srp_key, response, error);
				if (outcome == VerifyOutcome::Ok) {
					verified = true;
					saved_pairing = true;
				} else if (outcome == VerifyOutcome::Closed) {
					pairing_device_forget(device_id);
					if (result.error.empty())
						result.error =
							"The saved pairing was not accepted. Press Connect again and enter the code shown on the TV.";
					return result;
				} else {
					pairing_device_forget(device_id);
					secure_clear(srp_key);
					result.error.clear();
				}
			}
			if (!verified) {
				int last_status = 0;
				bool shown = false;
				std::vector<int> attempts;
				attempts.push_back(hkp);
				if (input.pin.empty()) {
					attempts.push_back(hkp == 5 ? 3 : 5);
					attempts.push_back(4);
				}
				for (int type : attempts) {
					hkp = type;
					std::map<std::string, std::string> headers =
						pin_headers(type, identity.id, true, false);
					std::vector<uint8_t> empty;
					note(result, "POST", "/pair-pin-start");
					if (!send_request(*io, "POST", uri_for(receiver, "/pair-pin-start"), cseq++,
							  &empty, "application/octet-stream", &headers, error)) {
						result.error = error;
						return result;
					}
					if (!reader.next(response, error)) {
						result.error = error;
						return result;
					}
					last_status = response.status;
					if (response.status == 200 || response.status == 453) {
						shown = true;
						break;
					}
					if (!input.pin.empty())
						break;
				}
				if (shown && input.request_pin)
					password = input.request_pin();
				else if (!input.pin.empty())
					password = input.pin;
				else if (!shown) {
					result.error = "The TV did not show a pairing code (RTSP " +
						       std::to_string(last_status) + ").";
					return result;
				}
				if (password.empty()) {
					result.error = kCodeRequired;
					return result;
				}
				if (!finish_pin_setup(result, *io, reader, receiver, cseq, hkp, identity, password,
						      srp_key, response, error, nullptr))
					return result;
				pairing_device_remember(device_id);
			}
		} else {
			std::map<std::string, std::string> hkp;
			hkp.emplace("X-Apple-HKP", "4");
			hkp.emplace("X-Apple-Client-Name", sender_name());
			std::map<std::string, std::string> start_headers = hkp;
			start_headers.emplace("X-Apple-SupportedPINLengths", "4");
			std::vector<uint8_t> empty;
			note(result, "POST", "/pair-pin-start");
			if (!send_request(*io, "POST", uri_for(receiver, "/pair-pin-start"), cseq++, &empty,
					  "application/octet-stream", &start_headers, error)) {
				result.error = error;
				return result;
			}
			if (!reader.next(response, error)) {
				result.error = error;
				return result;
			}
			password = input.pin.empty() ? "3939" : input.pin;
			std::vector<uint8_t> m1 = pair_setup_m1();
			if (!post_octet(result, *io, reader, receiver, "/pair-setup", cseq, m1, hkp, response, error)) {
				result.error = error;
				return result;
			}
			if (response.status != 200) {
				result.error = "Pairing failed (RTSP " + std::to_string(response.status) + ").";
				return result;
			}
			if (tlv_error_is(response.body, kTlvErrorAuthentication)) {
				result.error = kCodeRejected;
				return result;
			}
			std::vector<uint8_t> salt;
			std::vector<uint8_t> server_pub;
			if (!tlv_get(response.body, (uint8_t)TlvType::Salt, salt) ||
			    !tlv_get(response.body, (uint8_t)TlvType::PublicKey, server_pub)) {
				result.error = "Pairing failed.";
				return result;
			}
			SrpClient client;
			std::vector<uint8_t> client_proof;
			if (!client.start(password, error) || !client.proof(salt, server_pub, client_proof, error)) {
				result.error = "Pairing failed.";
				return result;
			}
			TlvItem state_item;
			state_item.type = (uint8_t)TlvType::State;
			state_item.value.push_back(3);
			TlvItem public_item;
			public_item.type = (uint8_t)TlvType::PublicKey;
			public_item.value = client.public_key();
			TlvItem proof_item;
			proof_item.type = (uint8_t)TlvType::Proof;
			proof_item.value = std::move(client_proof);
			std::vector<uint8_t> m3 = tlv_encode({state_item, public_item, proof_item});
			if (!post_octet(result, *io, reader, receiver, "/pair-setup", cseq, m3, hkp, response, error)) {
				result.error = error;
				return result;
			}
			if (response.status != 200) {
				result.error = "Pairing failed (RTSP " + std::to_string(response.status) + ").";
				return result;
			}
			if (tlv_error_is(response.body, kTlvErrorAuthentication)) {
				result.error = kCodeRejected;
				return result;
			}
			std::vector<uint8_t> server_proof;
			if (!tlv_get(response.body, (uint8_t)TlvType::Proof, server_proof) ||
			    !client.verify_server(server_proof)) {
				result.error = "Pairing failed.";
				return result;
			}
			srp_key = client.session_key();
		}
		sealed = std::make_unique<SealedControl>(transport, srp_key, false);
		io = sealed.get();
		reader.transport = io;
		if (!reader.buffer.empty()) {
			result.error = "Pairing failed.";
			return result;
		}
	}

	if (requires_fairplay(receiver.features, pair) && !have_ekey && !fairplay_absent) {
		/* No FairPlay bits: a 404 is the end of SAP, and the mirror continues. */
		if (!exchange_fairplay(!fairplay_bits))
			return result;
	}

	std::vector<uint8_t> rsa_wrapped;
	std::string wrap_error;
	bool have_rsa = false;
	if (!have_ekey)
		have_rsa = rsa_oaep_wrap_key(receiver.pk_hex, raw_key, rsa_wrapped, wrap_error);

	std::string session_id = uuid_string();
	std::map<std::string, Plist> session_dict;
	/* The working Mac mirror answers classic NTP timing (0xd2/0xd3), not PTP. */
	session_dict.emplace("timingProtocol", Plist::string("NTP"));
	session_dict.emplace("timingPort", Plist::integer(input.timing_port));
	std::map<std::string, Plist> peer;
	peer.emplace("ID", Plist::string(session_id));
	peer.emplace("SupportsClockPortMatchingOverride", Plist::boolean(true));
	peer.emplace("DeviceType", Plist::integer(0));
	std::string address = input.local_address;
	if (address.empty())
		address = receiver.address.empty() ? "127.0.0.1" : receiver.address;
	peer.emplace("Addresses", Plist::arr({Plist::string(address)}));
	session_dict.emplace("timingPeerInfo", Plist::dictionary(peer));
	session_dict.emplace("timingPeerList", Plist::arr({Plist::dictionary(std::move(peer))}));
	session_dict.emplace("sessionUUID", Plist::string(session_id));
	session_dict.emplace("isScreenMirroringSession", Plist::boolean(true));
	session_dict.emplace("osName", Plist::string("Mac OS X"));
	session_dict.emplace("osVersion", Plist::string("15.0"));
	session_dict.emplace("sourceVersion", Plist::string("960.13.1"));
	session_dict.emplace("name", Plist::string(sender_name()));
	std::string mac = sender_mac(session_id);
	session_dict.emplace("deviceID", Plist::string(mac));
	session_dict.emplace("macAddress", Plist::string(mac));
	session_dict.emplace("model", Plist::string("MacBookPro17,1"));
	session_dict.emplace("updateSessionRequest", Plist::boolean(false));
	bool session_encrypt = have_ekey || have_rsa;
	/* A missing key must omit et. An explicit 0 makes this receiver fail RECORD. */
	if (session_encrypt)
		session_dict.emplace("et", Plist::integer(32));
	if (have_ekey) {
		session_dict.emplace("ekey", Plist::blob(ekey));
		session_dict.emplace("eiv", Plist::blob(std::vector<uint8_t>(raw_iv, raw_iv + 16)));
	} else if (have_rsa) {
		session_dict.emplace("ekey", Plist::blob(std::move(rsa_wrapped)));
		session_dict.emplace("eiv", Plist::blob(std::vector<uint8_t>(raw_iv, raw_iv + 16)));
	}
	std::vector<uint8_t> session_body;
	if (!encode_bplist(Plist::dictionary(session_dict), session_body, error)) {
		result.error = error;
		return result;
	}
	std::string stream_uri = uri_for(receiver, "/stream");
	bool skip_record = false;
	note(result, "SETUP", "session");
	if (!send_request(*io, "SETUP", stream_uri, cseq++, &session_body, "application/x-apple-binary-plist", nullptr,
			  error)) {
		result.error = error;
		return result;
	}
	if (!reader.next(response, error)) {
		result.error = error;
		return result;
	}
	if (auth_failure(response.status)) {
		result.error = auth_error(response.status);
		return result;
	}
	if (response.status != 200) {
		result.error = "Session SETUP failed (RTSP " + std::to_string(response.status) + ")";
		return result;
	}
	if (!response.body.empty()) {
		Plist body;
		std::string plist_error;
		if (decode_plist(response.body.data(), response.body.size(), body, plist_error)) {
			result.video_port = port_from(body, "dataPort");
			observe_setup_ports(body, result, input, "session");
			if (body.get_bool("skipRecord"))
				skip_record = true;
		}
	}

	/* Screen audio is set up once the session is recording. A Mac receiver only
	   starts reading its audio sockets after RECORD. Returns false only when the
	   control connection fails; a refused stream leaves the mirror video-only. */
	auto setup_audio = [&]() -> bool {
		uint64_t audio_id = 0;
		random_bytes(&audio_id, sizeof audio_id);
		audio_id &= 0x7fffffffffffffffULL;
		if (!audio_id)
			audio_id = 1;
		std::vector<uint8_t> audio_key;
		if (!srp_key.empty()) {
			std::string salt = "DataStream-Salt" + std::to_string(audio_id);
			const char *info = "DataStream-Output-Encryption-Key";
			uint8_t derived[32];
			if (hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(salt.data()),
					salt.size(), reinterpret_cast<const uint8_t *>(info), std::strlen(info),
					derived, sizeof derived))
				audio_key.assign(derived, derived + sizeof derived);
			secure_clear(derived, sizeof derived);
		}
		int sync_port = input.audio_sync_port > 0 ? input.audio_sync_port : input.timing_port;
		std::vector<uint8_t> audio_body;
		if (!audio_setup_body(audio_id, audio_key, sync_port, audio_body, error))
			return true;
		note(result, "SETUP", "audio");
		if (!send_request(*io, "SETUP", stream_uri, cseq++, &audio_body, "application/x-apple-binary-plist",
				  nullptr, error) ||
		    !reader.next(response, error)) {
			result.error = error;
			return false;
		}
		if (response.status != 200 || response.body.empty())
			return true;
		Plist body;
		std::string plist_error;
		if (!decode_plist(response.body.data(), response.body.size(), body, plist_error))
			return true;
		observe_setup_ports(body, result, input, "audio");
		int port = 0;
		int control = 0;
		audio_ports_from(body, port, control);
		if (control == input.audio_data_port || control == input.audio_sync_port ||
		    control == input.timing_port)
			control = 0;
		if (port > 0 && port != input.audio_data_port && port != input.audio_sync_port &&
		    port != input.timing_port) {
			result.audio_negotiated = true;
			result.audio_port = port;
			result.audio_control_port = control;
			result.audio_chacha_key = std::move(audio_key);
		}
		return true;
	};

	uint64_t connection_id = 0;
	random_bytes(&connection_id, sizeof connection_id);
	connection_id &= 0x7fffffffffffffffULL;
	if (!connection_id)
		connection_id = 1;
	std::map<std::string, Plist> stream;
	stream.emplace("type", Plist::integer(110));
	stream.emplace("streamConnectionID", Plist::integer((int64_t)connection_id));
	stream.emplace("latencyMs", Plist::integer(75));
	stream.emplace("codecName", Plist::string(result.config.codec == Codec::Hevc ? "HEVC" : "H264"));
	stream.emplace("timestampInfo", name_array({"SubSu", "BePxT", "AfPxT", "BefEn", "EmEnc"}));
	if (have_ekey && !srp_key.empty()) {
		const uint8_t *parts[] = {raw_key, srp_key.data()};
		size_t sizes[] = {sizeof raw_key, srp_key.size()};
		uint8_t digest[64];
		sha512_parts(parts, sizes, 2, digest);
		stream.emplace("shk", Plist::blob(std::vector<uint8_t>(digest, digest + 16)));
		stream.emplace("shiv", Plist::blob(std::vector<uint8_t>(raw_iv, raw_iv + 16)));
		std::string salt = "DataStream-Salt" + std::to_string(connection_id);
		const char *info = "DataStream-Output-Encryption-Key";
		uint8_t chacha[32];
		if (!hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(salt.data()),
				 salt.size(), reinterpret_cast<const uint8_t *>(info), std::strlen(info), chacha,
				 sizeof chacha)) {
			result.error = kFairPlayFailed;
			return result;
		}
		result.use_chacha = true;
		result.chacha_key.assign(chacha, chacha + sizeof chacha);
		secure_clear(digest, sizeof digest);
	} else if (fairplay_absent && !srp_key.empty()) {
		/* No FairPlay route. The stream descriptor carries the front of the
		 * control keys, and the video cipher uses the pair-verify secret.
		 */
		const char *control_salt = "Control-Salt";
		uint8_t write_key[32];
		uint8_t read_key[32];
		if (!hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(control_salt),
				 std::strlen(control_salt),
				 reinterpret_cast<const uint8_t *>("Control-Write-Encryption-Key"),
				 std::strlen("Control-Write-Encryption-Key"), write_key, sizeof write_key) ||
		    !hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(control_salt),
				 std::strlen(control_salt),
				 reinterpret_cast<const uint8_t *>("Control-Read-Encryption-Key"),
				 std::strlen("Control-Read-Encryption-Key"), read_key, sizeof read_key)) {
			result.error = "Pairing failed.";
			return result;
		}
		stream.emplace("shk", Plist::blob(std::vector<uint8_t>(write_key, write_key + 16)));
		stream.emplace("shiv", Plist::blob(std::vector<uint8_t>(read_key, read_key + 16)));
		secure_clear(write_key, sizeof write_key);
		secure_clear(read_key, sizeof read_key);
		std::string salt = "DataStream-Salt" + std::to_string(connection_id);
		const char *info = "DataStream-Output-Encryption-Key";
		uint8_t chacha[32];
		if (!hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(salt.data()),
				 salt.size(), reinterpret_cast<const uint8_t *>(info), std::strlen(info), chacha,
				 sizeof chacha)) {
			result.error = "Pairing failed.";
			return result;
		}
		result.use_chacha = true;
		result.chacha_key.assign(chacha, chacha + sizeof chacha);
		secure_clear(chacha, sizeof chacha);
	}
	std::map<std::string, Plist> streams;
	streams.emplace("streams", Plist::arr({Plist::dictionary(std::move(stream))}));
	std::vector<uint8_t> video_body;
	if (!encode_bplist(Plist::dictionary(streams), video_body, error)) {
		result.error = error;
		return result;
	}
	note(result, "SETUP", "video");
	if (!send_request(*io, "SETUP", stream_uri, cseq++, &video_body, "application/x-apple-binary-plist", nullptr,
			  error)) {
		result.error = error;
		return result;
	}
	if (!reader.next(response, error)) {
		result.error = error;
		return result;
	}
	if (codec_rejected(response.status) && result.config.codec == Codec::Hevc) {
		if (!sealed)
			retry_h264 = true;
		result.error = "Receiver rejected HEVC (RTSP " + std::to_string(response.status) + ")";
		return result;
	}
	if (auth_failure(response.status)) {
		result.error = auth_error(response.status);
		return result;
	}
	if (response.status != 200) {
		result.error = "Video SETUP failed (RTSP " + std::to_string(response.status) + ")";
		return result;
	}
	if (!response.body.empty()) {
		Plist body;
		std::string plist_error;
		if (decode_plist(response.body.data(), response.body.size(), body, plist_error)) {
			int port = port_from(body, "dataPort");
			if (port > 0)
				result.video_port = port;
			observe_setup_ports(body, result, input, "video");
			if (body.get_bool("skipRecord"))
				skip_record = true;
		}
	}

	if (result.event_port > 0 && input.prepare_event) {
		std::string prep_error;
		if (!input.prepare_event(result.event_port, prep_error)) {
			result.error = prep_error.empty() ? "Could not open the event channel" : prep_error;
			return result;
		}
	}
	if (result.video_port > 0 && input.prepare_video) {
		std::string prep_error;
		if (!input.prepare_video(result.video_port, prep_error)) {
			result.error = prep_error.empty() ? "Could not open the video stream" : prep_error;
			return result;
		}
	}
	if (!skip_record) {
		std::map<std::string, std::string> record_headers;
		record_headers.emplace("Session", session_id);
		record_headers.emplace("Range", "npt=0-");
		record_headers.emplace("RTP-Info", "seq=0;rtptime=0");
		note(result, "RECORD", "/stream");
		if (!send_request(*io, "RECORD", stream_uri, cseq++, nullptr, nullptr, &record_headers, error) ||
		    !reader.next(response, error)) {
			result.error = error;
			return result;
		}
		if (auth_failure(response.status)) {
			result.error = auth_error(response.status);
			return result;
		}
		if (response.status != 200) {
			result.error = "RECORD failed (RTSP " + std::to_string(response.status) + ")" +
				       response_hint(response);
			return result;
		}
	}

	if (input.send_audio && !setup_audio())
		return result;

	/* The pairing secret is a SHA-512 digest, 64 bytes. HKDF accepts that length. */
	if (!srp_key.empty()) {
		const char *event_salt = "Events-Salt";
		uint8_t event_read[32];
		uint8_t event_write[32];
		if (hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(event_salt),
				std::strlen(event_salt),
				reinterpret_cast<const uint8_t *>("Events-Write-Encryption-Key"),
				std::strlen("Events-Write-Encryption-Key"), event_read, sizeof event_read) &&
		    hkdf_sha512(srp_key.data(), srp_key.size(), reinterpret_cast<const uint8_t *>(event_salt),
				std::strlen(event_salt),
				reinterpret_cast<const uint8_t *>("Events-Read-Encryption-Key"),
				std::strlen("Events-Read-Encryption-Key"), event_write, sizeof event_write)) {
			result.event_read_key.assign(event_read, event_read + sizeof event_read);
			result.event_write_key.assign(event_write, event_write + sizeof event_write);
		}
		secure_clear(event_read, sizeof event_read);
		secure_clear(event_write, sizeof event_write);
	}

	if (result.use_chacha) {
		/* ChaCha seals the video payloads. AES-CTR stays off on this path. */
	} else if (session_encrypt) {
		result.encrypt = true;
		result.aes_key.assign(raw_key, raw_key + sizeof raw_key);
		result.aes_iv.assign(raw_iv, raw_iv + sizeof raw_iv);
	}
	std::ostringstream status;
	status << "Streaming " << codec_name(result.config.codec) << ' ' << result.config.width << 'x'
	       << result.config.height << '@' << result.config.fps << ", " << (result.config.bitrate / 1000) << " kbps";
	if (result.config.fell_back_from_hevc)
		status << ", HEVC was rejected";
	if (saved_pairing)
		status << ", saved pairing";
	status << ((result.encrypt || result.use_chacha) ? ", encrypted" : ", unencrypted");
	if (input.send_audio && !result.audio_negotiated)
		status << ". Receiver did not accept audio; sending video only";
	else if (result.audio_negotiated) {
		status << ", audio on port " << result.audio_port;
		if (result.audio_control_port > 0)
			status << ", sync " << result.audio_control_port;
	}
	result.status = status.str();
	result.next_cseq = cseq;
	if (retain_seal && sealed)
		*retain_seal = std::move(sealed);
	result.ok = true;
	return result;
}

bool write_feedback(ByteTransport &transport, int cseq, std::string &error)
{
	std::map<std::string, std::string> extra;
	extra.emplace("Content-Length", "0");
	return send_request(transport, "POST", "/feedback", cseq, nullptr, nullptr, &extra, error);
}

bool read_feedback(ByteTransport &transport, std::vector<uint8_t> &pending, const std::atomic<bool> *stop,
		   RtspMessage &response, std::string &error)
{
	for (int spins = 0; spins < 6; spins++) {
		if (stop && stop->load()) {
			error = "Stopped";
			return false;
		}
		size_t consumed = 0;
		if (parse_rtsp_message(pending.data(), pending.size(), response, consumed)) {
			pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)consumed);
			return true;
		}
		uint8_t chunk[4096];
		int count = transport.read_some(chunk, sizeof chunk, 500);
		if (count < 0) {
			error = "The receiver closed the connection";
			return false;
		}
		if (count == 0)
			continue;
		pending.insert(pending.end(), chunk, chunk + count);
	}
	error = "Timed out waiting for the receiver";
	return false;
}

} /* namespace */

bool audio_setup_body(uint64_t stream_id, const std::vector<uint8_t> &shk, int control_port, std::vector<uint8_t> &body,
		      std::string &error)
{
	std::map<std::string, Plist> audio;
	audio.emplace("type", Plist::integer(96));
	audio.emplace("streamConnectionID", Plist::integer((int64_t)stream_id));
	if (!shk.empty())
		audio.emplace("shk", Plist::blob(shk));
	/* The mirror dump uses 44.1 kHz. At that rate the audio slowly fell behind the picture. */
	audio.emplace("ct", Plist::integer(8));
	audio.emplace("spf", Plist::integer(480));
	audio.emplace("sr", Plist::integer(kAudioRate));
	audio.emplace("audioFormat", Plist::integer(kAudioRate == 48000 ? 0x2000000 : 0x1000000));
	audio.emplace("audioMode", Plist::string("default"));
	audio.emplace("usingScreen", Plist::boolean(true));
	audio.emplace("isMedia", Plist::boolean(false));
	/* The Mac's audio SETUP is 312 bytes: shk and a control port, no streamConnections. */
	audio.emplace("controlPort", Plist::integer(control_port));
	audio.emplace("latencyMin", Plist::integer(kAudioRate * 17 / 100));
	audio.emplace("latencyMax", Plist::integer(kAudioRate * 2));
	std::map<std::string, Plist> root;
	root.emplace("streams", Plist::arr({Plist::dictionary(std::move(audio))}));
	return encode_bplist(Plist::dictionary(root), body, error);
}

bool post_feedback(ByteTransport &transport, int &cseq, std::string &error)
{
	if (!write_feedback(transport, cseq, error))
		return false;
	cseq++;
	std::vector<uint8_t> pending;
	RtspMessage response;
	if (!read_feedback(transport, pending, nullptr, response, error))
		return false;
	if (response.status != 200) {
		error = "Feedback failed (RTSP " + std::to_string(response.status) + ")";
		return false;
	}
	return true;
}

bool answer_event(ByteTransport &transport, std::vector<uint8_t> &pending, const std::atomic<bool> *stop,
		  std::string &error)
{
	for (int spins = 0; spins < 8; spins++) {
		if (stop && stop->load()) {
			error = "Stopped";
			return false;
		}
		RtspMessage request;
		size_t consumed = 0;
		if (parse_rtsp_message(pending.data(), pending.size(), request, consumed)) {
			pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)consumed);
			std::string cseq = header_get(request, "CSeq");
			if (cseq.empty())
				cseq = "0";
			std::string response = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\nContent-Length: 0\r\n\r\n";
			if (!transport.write_all(reinterpret_cast<const uint8_t *>(response.data()), response.size())) {
				error = "The event connection closed";
				return false;
			}
			return true;
		}
		uint8_t chunk[2048];
		int count = transport.read_some(chunk, sizeof chunk, 200);
		if (count < 0) {
			error = "The event connection closed";
			return false;
		}
		if (count == 0)
			continue;
		pending.insert(pending.end(), chunk, chunk + count);
	}
	error = "Timed out waiting for an event";
	return false;
}

void pump_events(ByteTransport &transport, const std::atomic<bool> &stop)
{
	std::vector<uint8_t> pending;
	std::string error;
	while (!stop.load()) {
		if (!answer_event(transport, pending, &stop, error)) {
			if (error == "The event connection closed" || error == "Stopped" || stop.load())
				return;
		}
	}
}

void pump_feedback(ByteTransport &transport, int cseq, const std::atomic<bool> &stop)
{
	std::vector<uint8_t> pending;
	auto send = [&](std::string &error) {
		if (!write_feedback(transport, cseq, error))
			return false;
		cseq++;
		return true;
	};
	std::string error;
	if (stop.load() || !send(error))
		return;
	while (!stop.load()) {
		RtspMessage response;
		if (!read_feedback(transport, pending, &stop, response, error)) {
			if (error == "The receiver closed the connection" || error == "Stopped")
				return;
		}
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!stop.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (stop.load() || !send(error))
			return;
	}
}

HandshakeResult handshake(ByteTransport &transport, const HandshakeInput &input,
			  std::unique_ptr<SealedControl> *retain_seal)
{
	bool retry = false;
	HandshakeResult first = run_once(transport, input, input.allow_hevc, retry, retain_seal);
	if (first.ok || !retry)
		return first;
	if (retain_seal)
		retain_seal->reset();
	HandshakeInput again = input;
	bool ignored = false;
	HandshakeResult second = run_once(transport, again, false, ignored, retain_seal);
	if (second.ok) {
		second.config.fell_back_from_hevc = true;
		second.status += ", HEVC was rejected";
	} else if (second.error.empty()) {
		second.error = first.error;
	}
	return second;
}

} /* namespace airplay */
