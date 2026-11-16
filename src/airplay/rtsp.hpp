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

struct RtspMessage {
	int status = 0;
	std::string reason;
	std::map<std::string, std::string> headers;
	std::vector<uint8_t> body;
	std::string headline;
};

std::string header_get(const RtspMessage &message, const std::string &name);

bool parse_rtsp_message(const uint8_t *data, size_t len, RtspMessage &out, size_t &consumed);

std::string make_rtsp_request(const std::string &method, const std::string &uri, int cseq,
			      const std::map<std::string, std::string> &headers, const uint8_t *body, size_t body_len);

} /* namespace airplay */
