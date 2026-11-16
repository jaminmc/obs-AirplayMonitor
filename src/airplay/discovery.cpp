/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A small mDNS browser. Queries go to the multicast group from an ephemeral port, which RFC 6762
 * section 6.7 has responders answer by unicast. That needs no system Bonjour or Avahi service and
 * never competes with one for port 5353.
 */

#include "airplay/discovery.hpp"

#include "airplay/net.hpp"
#include "airplay/portable.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace airplay {
namespace {

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypePtr = 12;
constexpr uint16_t kTypeTxt = 16;
constexpr uint16_t kTypeAaaa = 28;
constexpr uint16_t kTypeSrv = 33;
const char kService[] = "_airplay._tcp.local.";

std::string lower(std::string text)
{
	for (char &ch : text)
		ch = (char)std::tolower((unsigned char)ch);
	return text;
}

uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

/* Reads a possibly compressed name starting at `pos`. On return `pos` is past the name in place. */
bool read_name(const uint8_t *packet, size_t size, size_t &pos, std::string &name, std::string *first_label = nullptr)
{
	name.clear();
	size_t at = pos;
	bool jumped = false;
	int hops = 0;
	bool first = true;
	while (true) {
		if (at >= size)
			return false;
		uint8_t length = packet[at];
		if ((length & 0xc0) == 0xc0) {
			if (at + 1 >= size || ++hops > 16)
				return false;
			size_t target = (size_t)(length & 0x3f) << 8 | packet[at + 1];
			if (!jumped)
				pos = at + 2;
			jumped = true;
			at = target;
			continue;
		}
		if (length & 0xc0)
			return false;
		at++;
		if (length == 0)
			break;
		if (at + length > size)
			return false;
		std::string label(reinterpret_cast<const char *>(packet + at), length);
		if (first && first_label)
			*first_label = label;
		first = false;
		name += label;
		name += '.';
		at += length;
	}
	if (!jumped)
		pos = at;
	if (name.empty())
		name = ".";
	return true;
}

void put_name(std::vector<uint8_t> &out, const std::string &name)
{
	size_t start = 0;
	while (start < name.size()) {
		size_t dot = name.find('.', start);
		if (dot == std::string::npos)
			dot = name.size();
		size_t length = std::min<size_t>(dot - start, 63);
		if (length) {
			out.push_back((uint8_t)length);
			out.insert(out.end(), name.begin() + (std::ptrdiff_t)start,
				   name.begin() + (std::ptrdiff_t)(start + length));
		}
		start = dot + 1;
	}
	out.push_back(0);
}

std::string txt_value(const std::vector<uint8_t> &txt, const char *key)
{
	size_t index = 0;
	size_t key_len = std::strlen(key);
	while (index < txt.size()) {
		uint8_t length = txt[index++];
		if (index + length > txt.size())
			break;
		if (length > key_len && std::memcmp(txt.data() + index, key, key_len) == 0 &&
		    txt[index + key_len] == '=') {
			return std::string(reinterpret_cast<const char *>(txt.data() + index + key_len + 1),
					   length - key_len - 1);
		}
		index += length;
	}
	return {};
}

} /* namespace */

std::vector<uint8_t> mdns_query(uint16_t id, const std::vector<std::pair<std::string, uint16_t>> &questions)
{
	std::vector<uint8_t> out = {
		(uint8_t)(id >> 8), (uint8_t)id, 0, 0, 0, (uint8_t)questions.size(), 0, 0, 0, 0, 0, 0};
	for (const auto &question : questions) {
		put_name(out, question.first);
		out.push_back((uint8_t)(question.second >> 8));
		out.push_back((uint8_t)question.second);
		/* Class IN with the unicast-response bit. */
		out.push_back(0x80);
		out.push_back(0x01);
	}
	return out;
}

std::vector<MdnsRecord> mdns_parse(const uint8_t *packet, size_t size)
{
	std::vector<MdnsRecord> records;
	if (!packet || size < 12 || !(packet[2] & 0x80))
		return records;
	size_t questions = be16(packet + 4);
	size_t answers = (size_t)be16(packet + 6) + be16(packet + 8) + be16(packet + 10);
	size_t pos = 12;
	std::string name;
	for (size_t i = 0; i < questions; i++) {
		if (!read_name(packet, size, pos, name) || pos + 4 > size)
			return records;
		pos += 4;
	}
	for (size_t i = 0; i < answers; i++) {
		MdnsRecord record;
		if (!read_name(packet, size, pos, name) || pos + 10 > size)
			return records;
		record.name = lower(name);
		record.type = be16(packet + pos);
		size_t length = be16(packet + pos + 8);
		pos += 10;
		if (pos + length > size)
			return records;
		const uint8_t *data = packet + pos;
		size_t inner = pos;
		switch (record.type) {
		case kTypePtr:
			if (!read_name(packet, size, inner, name, &record.instance))
				return records;
			record.target = lower(name);
			break;
		case kTypeSrv:
			if (length < 7)
				break;
			record.port = be16(data + 4);
			inner += 6;
			if (!read_name(packet, size, inner, name))
				return records;
			record.target = lower(name);
			break;
		case kTypeTxt:
			record.txt.assign(data, data + length);
			break;
		case kTypeA:
		case kTypeAaaa: {
			char text[INET6_ADDRSTRLEN] = {};
			if (record.type == kTypeA && length == 4)
				inet_ntop(AF_INET, data, text, sizeof text);
			else if (record.type == kTypeAaaa && length == 16)
				inet_ntop(AF_INET6, data, text, sizeof text);
			record.address = text;
			break;
		}
		default:
			break;
		}
		pos += length;
		records.push_back(std::move(record));
	}
	return records;
}

struct Discovery::Impl {
	std::thread thread;
	std::atomic<bool> stop{false};
	Callback callback;

	struct Instance {
		std::string display;
		std::string host;
		uint16_t port = 0;
		std::vector<uint8_t> txt;
		bool has_txt = false;
		std::chrono::steady_clock::time_point asked{};
	};
	struct Host {
		std::string ipv4;
		std::string ipv6;
		std::chrono::steady_clock::time_point asked{};
	};
	std::map<std::string, Instance> instances;
	std::map<std::string, Host> hosts;
	std::map<std::string, std::string> published;

	void absorb(const std::vector<MdnsRecord> &records)
	{
		for (const MdnsRecord &record : records) {
			if (record.type == kTypePtr && record.name == kService && !record.target.empty()) {
				Instance &instance = instances[record.target];
				if (!record.instance.empty())
					instance.display = record.instance;
			}
		}
		for (const MdnsRecord &record : records) {
			auto instance = instances.find(record.name);
			if (record.type == kTypeSrv && instance != instances.end()) {
				instance->second.host = record.target;
				instance->second.port = record.port;
			} else if (record.type == kTypeTxt && instance != instances.end()) {
				instance->second.txt = record.txt;
				instance->second.has_txt = true;
			} else if (record.type == kTypeA && !record.address.empty()) {
				hosts[record.name].ipv4 = record.address;
			} else if (record.type == kTypeAaaa && !record.address.empty()) {
				Host &host = hosts[record.name];
				/* A link-local address needs a scope this code does not track. */
				if (host.ipv6.empty() || lower(host.ipv6).rfind("fe80", 0) == 0)
					host.ipv6 = record.address;
			}
		}
	}

	/* Questions for whatever is still missing, at most once a second per name. */
	std::vector<std::pair<std::string, uint16_t>> follow_ups()
	{
		std::vector<std::pair<std::string, uint16_t>> questions;
		auto now = std::chrono::steady_clock::now();
		for (auto &entry : instances) {
			Instance &instance = entry.second;
			bool need_srv = instance.host.empty();
			bool need_txt = !instance.has_txt;
			if ((need_srv || need_txt) && now - instance.asked > std::chrono::seconds(1)) {
				instance.asked = now;
				if (need_srv)
					questions.emplace_back(entry.first, kTypeSrv);
				if (need_txt)
					questions.emplace_back(entry.first, kTypeTxt);
			}
			if (instance.host.empty())
				continue;
			Host &host = hosts[instance.host];
			if (host.ipv4.empty() && host.ipv6.empty() && now - host.asked > std::chrono::seconds(1)) {
				host.asked = now;
				questions.emplace_back(instance.host, kTypeA);
				questions.emplace_back(instance.host, kTypeAaaa);
			}
		}
		return questions;
	}

	void publish_ready()
	{
		std::vector<DiscoveredReceiver> devices;
		std::map<std::string, std::string> signature;
		for (const auto &entry : instances) {
			const Instance &instance = entry.second;
			auto host = hosts.find(instance.host);
			if (instance.host.empty() || !instance.has_txt || host == hosts.end())
				continue;
			DiscoveredReceiver device;
			device.name = instance.display.empty() ? entry.first : instance.display;
			device.host = instance.host;
			device.port = instance.port;
			device.model = txt_value(instance.txt, "model");
			device.device_id = txt_value(instance.txt, "deviceid");
			device.pk_hex = txt_value(instance.txt, "pk");
			device.srcvers = txt_value(instance.txt, "srcvers");
			device.features = parse_features_txt(txt_value(instance.txt, "features"));
			device.status_flags = (uint32_t)parse_features_txt(txt_value(instance.txt, "flags"));
			device.reason = reject_reason(device.features);
			if (!host->second.ipv4.empty()) {
				device.address = host->second.ipv4;
				device.family = AF_INET;
			} else if (!host->second.ipv6.empty()) {
				device.address = host->second.ipv6;
				device.family = AF_INET6;
			} else {
				continue;
			}
			signature[entry.first] = device.address + "|" + std::to_string(device.port) + "|" +
						 std::string(instance.txt.begin(), instance.txt.end());
			devices.push_back(std::move(device));
		}
		if (signature == published)
			return;
		published = std::move(signature);
		std::sort(devices.begin(), devices.end(),
			  [](const DiscoveredReceiver &a, const DiscoveredReceiver &b) { return a.name < b.name; });
		if (callback)
			callback(std::move(devices));
	}

	void run()
	{
		net::Socket fd = net::open(AF_INET, SOCK_DGRAM);
		if (!net::valid(fd))
			return;
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof local) != 0) {
			net::close(fd);
			return;
		}
		net::set_int_option(fd, IPPROTO_IP, IP_MULTICAST_TTL, 255);
		sockaddr_in group{};
		group.sin_family = AF_INET;
		group.sin_port = htons(5353);
		inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
		auto send = [&](const std::vector<std::pair<std::string, uint16_t>> &questions) {
			if (questions.empty())
				return;
			uint16_t id = 0;
			random_bytes(&id, sizeof id);
			std::vector<uint8_t> query = mdns_query(id, questions);
			net::send_to(fd, query.data(), query.size(), reinterpret_cast<sockaddr *>(&group),
				     sizeof group);
		};

		const auto started = std::chrono::steady_clock::now();
		const int schedule_ms[] = {0, 1000, 3000, 7000};
		size_t next_scheduled = 0;
		auto next_browse = started;
		while (!stop.load()) {
			auto now = std::chrono::steady_clock::now();
			if (now >= next_browse) {
				send({{kService, kTypePtr}});
				next_scheduled++;
				next_browse = next_scheduled < sizeof schedule_ms / sizeof schedule_ms[0]
						      ? started + std::chrono::milliseconds(schedule_ms[next_scheduled])
						      : now + std::chrono::seconds(15);
			}
			if (net::wait(fd, false, 200)) {
				uint8_t packet[9000];
				sockaddr_storage from;
				socklen_t from_len = sizeof from;
				net::ssize count = net::recv_from(fd, packet, sizeof packet,
								  reinterpret_cast<sockaddr *>(&from), &from_len);
				if (count > 0)
					absorb(mdns_parse(packet, (size_t)count));
			}
			send(follow_ups());
			publish_ready();
		}
		net::close(fd);
	}
};

Discovery::Discovery() : impl_(new Impl) {}

Discovery::~Discovery()
{
	stop();
	delete impl_;
}

void Discovery::stop()
{
	impl_->stop.store(true);
	if (impl_->thread.joinable())
		impl_->thread.join();
}

void Discovery::start(Callback callback)
{
	stop();
	impl_->stop.store(false);
	impl_->callback = std::move(callback);
	impl_->instances.clear();
	impl_->hosts.clear();
	impl_->published.clear();
	impl_->thread = std::thread([this]() { impl_->run(); });
}

} /* namespace airplay */
