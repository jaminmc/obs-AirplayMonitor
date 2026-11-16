/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/features.hpp"
#include "airplay/handshake.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace airplay {

struct VideoAu {
	std::vector<uint8_t> data;
	std::vector<uint8_t> codec_config;
	std::vector<uint8_t> sample_description;
	bool keyframe = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t encode_ns = 0;
	double session_seconds = 0;
};

class Session {
public:
	using StatusFn = std::function<void(const std::string &)>;
	using ReadyFn = std::function<void(const HandshakeResult &)>;

	Session();
	~Session();

	void start(const HandshakeInput &input, StatusFn on_status, ReadyFn on_ready);
	void stop();
	bool waiting_for_pin() const;
	void submit_pin(std::string pin);
	bool running() const;
	bool streaming() const;
	void submit_video(VideoAu au);
	/* capture_ns is when the packet's first sample was captured, on std::chrono::steady_clock.
	   Zero means the packet was made just now. */
	void submit_audio(std::vector<uint8_t> rtp, uint64_t capture_ns = 0);
	/* Shifts receiver audio against the picture. Positive plays it later, negative earlier. */
	void set_audio_offset_ms(int ms);
	uint64_t dropped() const;
	const HandshakeResult &result() const;

private:
	struct Impl;
	Impl *impl_;
};

} /* namespace airplay */
