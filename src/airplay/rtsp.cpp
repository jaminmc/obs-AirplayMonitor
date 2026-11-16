/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/rtsp.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace airplay {
namespace {

std::string lower_copy(std::string value)
{
	for (char &ch : value)
		ch = (char)std::tolower((unsigned char)ch);
	return value;
}

std::string trim(const std::string &value)
{
	size_t begin = 0;
	while (begin < value.size() && std::isspace((unsigned char)value[begin]))
		begin++;
	size_t end = value.size();
	while (end > begin && std::isspace((unsigned char)value[end - 1]))
		end--;
	return value.substr(begin, end - begin);
}

} /* namespace */

std::string header_get(const RtspMessage &message, const std::string &name)
{
	auto it = message.headers.find(lower_copy(name));
	if (it == message.headers.end())
		return {};
	return it->second;
}

bool parse_rtsp_message(const uint8_t *data, size_t len, RtspMessage &out, size_t &consumed)
{
	consumed = 0;
	if (!data || len < 12)
		return false;
	const char *text = reinterpret_cast<const char *>(data);
	const char *split = nullptr;
	for (size_t i = 0; i + 3 < len; i++) {
		if (text[i] == '\r' && text[i + 1] == '\n' && text[i + 2] == '\r' && text[i + 3] == '\n') {
			split = text + i;
			break;
		}
	}
	if (!split)
		return false;

	RtspMessage message;
	const char *line = text;
	bool first = true;
	while (line < split) {
		const char *nl = line;
		while (nl < split && *nl != '\n')
			nl++;
		std::string raw(line, (size_t)(nl - line));
		if (!raw.empty() && raw.back() == '\r')
			raw.pop_back();
		if (first) {
			message.headline = raw;
			first = false;
			if (raw.compare(0, 5, "RTSP/") == 0 || raw.compare(0, 5, "HTTP/") == 0) {
				auto space = raw.find(' ');
				if (space != std::string::npos)
					message.status = std::atoi(raw.c_str() + space + 1);
				auto reason = raw.find(' ', space == std::string::npos ? 0 : space + 1);
				if (reason != std::string::npos)
					message.reason = trim(raw.substr(reason + 1));
			}
		} else if (!raw.empty()) {
			auto colon = raw.find(':');
			if (colon != std::string::npos)
				message.headers[lower_copy(trim(raw.substr(0, colon)))] = trim(raw.substr(colon + 1));
		}
		line = nl < split ? nl + 1 : split;
	}

	size_t header_end = (size_t)(split - text) + 4;
	size_t body_len = 0;
	std::string content_length = header_get(message, "content-length");
	if (!content_length.empty())
		body_len = (size_t)std::strtoul(content_length.c_str(), nullptr, 10);
	if (header_end + body_len > len)
		return false;
	if (body_len)
		message.body.assign(data + header_end, data + header_end + body_len);
	consumed = header_end + body_len;
	out = std::move(message);
	return true;
}

std::string make_rtsp_request(const std::string &method, const std::string &uri, int cseq,
			      const std::map<std::string, std::string> &headers, const uint8_t *body, size_t body_len)
{
	std::string request;
	request.reserve(256 + body_len);
	request += method;
	request += ' ';
	request += uri;
	request += " RTSP/1.0\r\n";
	request += "CSeq: " + std::to_string(cseq) + "\r\n";
	/* The version a current Mac sends. Receivers read it to pick protocol features. */
	request += "User-Agent: AirPlay/960.13.1\r\n";
	for (const auto &header : headers) {
		request += header.first;
		request += ": ";
		request += header.second;
		request += "\r\n";
	}
	if (body && body_len && headers.find("Content-Length") == headers.end() &&
	    headers.find("Content-length") == headers.end())
		request += "Content-Length: " + std::to_string(body_len) + "\r\n";
	request += "\r\n";
	if (body && body_len)
		request.append(reinterpret_cast<const char *>(body), body_len);
	return request;
}

} /* namespace airplay */
