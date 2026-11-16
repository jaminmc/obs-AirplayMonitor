/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sends a test picture and a tone to a receiver without OBS, so the audio
 * path can be tried without rebuilding the plugin.
 */

#include "airplay/rtp_audio.hpp"
#include "airplay/session.hpp"
#include "audio/aac_eld.hpp"
#include "encode/vt_encoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

namespace {

/* One 100 ms pulse, a 2 s pause, two pulses 0.4 s apart, then another 2 s pause. */
bool sync_pulse(double t)
{
	constexpr double kCycle = 4.5;
	const double starts[] = {0.0, 2.1, 2.5};
	double phase = t - kCycle * (double)(int64_t)(t / kCycle);
	for (double start : starts) {
		if (phase >= start && phase < start + 0.1)
			return true;
	}
	return false;
}

std::vector<uint8_t> color_bars(uint32_t width, uint32_t height, int phase)
{
	const uint8_t y_values[] = {16, 235, 210, 170, 145, 106, 81, 16};
	const uint8_t u_values[] = {128, 128, 16, 166, 54, 202, 90, 128};
	const uint8_t v_values[] = {128, 128, 146, 16, 34, 222, 240, 128};
	std::vector<uint8_t> frame((size_t)width * height * 3 / 2, 16);
	uint8_t *y = frame.data();
	uint8_t *uv = y + (size_t)width * height;
	for (uint32_t row = 0; row < height; row++) {
		for (uint32_t col = 0; col < width; col++) {
			int bar = (int)(((col + (uint32_t)phase * 8) * 8) / width) & 7;
			y[(size_t)row * width + col] = y_values[bar];
		}
	}
	for (uint32_t row = 0; row < height / 2; row++) {
		for (uint32_t col = 0; col < width / 2; col++) {
			int bar = (int)(((col * 2 + (uint32_t)phase * 8) * 8) / width) & 7;
			uv[((size_t)row * width) + (size_t)col * 2] = u_values[bar];
			uv[((size_t)row * width) + (size_t)col * 2 + 1] = v_values[bar];
		}
	}
	return frame;
}

} /* namespace */

int main(int argc, char **argv)
{
	std::string address = "127.0.0.1";
	int seconds = 30;
	bool sync_test = false;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--seconds" && i + 1 < argc)
			seconds = std::atoi(argv[++i]);
		else if (arg == "--sync")
			sync_test = true;
		else if (arg.rfind("--", 0) != 0)
			address = arg;
	}
	if (seconds < 5)
		seconds = 5;

	airplay::HandshakeInput input;
	input.receiver.address = address;
	input.receiver.port = 7000;
	input.receiver.family = address.find(':') != std::string::npos ? AF_INET6 : AF_INET;
	input.receiver.name = "This Mac";
	input.send_audio = true;
	input.src_width = 1920;
	input.src_height = 1080;
	input.src_fps = 60;

	std::mutex mu;
	std::condition_variable cv;
	bool ready = false;
	bool failed = false;
	std::string status;
	airplay::HandshakeResult negotiated;
	airplay::Session session;
	std::fprintf(stderr, "Connecting to %s:7000. Approve AirPlay on this Mac if it asks.\n", address.c_str());
	session.start(
		input,
		[&](const std::string &text) {
			std::lock_guard<std::mutex> lock(mu);
			status = text;
			std::fprintf(stderr, "status: %s\n", text.c_str());
			if (text.find("Could not") != std::string::npos || text.find("failed") != std::string::npos ||
			    text.find("rejected") != std::string::npos || text.find("Disconnected") != std::string::npos)
				failed = true;
			cv.notify_all();
		},
		[&](const airplay::HandshakeResult &result) {
			std::lock_guard<std::mutex> lock(mu);
			negotiated = result;
			ready = true;
			cv.notify_all();
		});

	{
		std::unique_lock<std::mutex> lock(mu);
		cv.wait_for(lock, std::chrono::seconds(90), [&]() { return ready || failed || session.waiting_for_pin(); });
	}
	if (session.waiting_for_pin()) {
		std::fprintf(stderr, "Type the code shown on the receiver, then press return:\n");
		std::string pin;
		if (!std::getline(std::cin, pin)) {
			session.stop();
			return 1;
		}
		session.submit_pin(pin);
		std::unique_lock<std::mutex> lock(mu);
		cv.wait_for(lock, std::chrono::seconds(60), [&]() { return ready || failed; });
	}
	if (!ready) {
		{
			std::lock_guard<std::mutex> lock(mu);
			std::fprintf(stderr, "Did not start. %s\n", status.c_str());
		}
		session.stop();
		return 1;
	}

	std::string encoder_error;
	airplay::VtEncoder video;
	if (!video.start(negotiated.config, encoder_error)) {
		std::fprintf(stderr, "Video encoder: %s\n", encoder_error.c_str());
		session.stop();
		return 1;
	}
	airplay::AacEldEncoder audio;
	uint32_t rate = negotiated.audio_sample_rate ? negotiated.audio_sample_rate : 48000;
	uint32_t spf = negotiated.audio_frame_samples ? negotiated.audio_frame_samples : 480;
	if (negotiated.audio_negotiated && !audio.start(rate, encoder_error)) {
		std::fprintf(stderr, "Audio encoder: %s\n", encoder_error.c_str());
		session.stop();
		return 1;
	}
	std::fprintf(stderr, "Sending %dx%d %s at 60000/1001 and a 440 Hz AAC-ELD tone for %d seconds.\n",
		     negotiated.config.width, negotiated.config.height, airplay::codec_name(negotiated.config.codec),
		     seconds);

	uint16_t seq = 0;
	uint32_t ssrc = 0x41504d31;
	double tone = 0;
	uint64_t sample_index = 0;
	auto started = std::chrono::steady_clock::now();
	auto next_audio = started;
	auto next_video = started;
	const auto audio_step = std::chrono::microseconds(1000000 * spf / rate);
	constexpr uint64_t kRateNum = 1001;
	constexpr uint64_t kRateDen = 60000;
	const auto video_step = std::chrono::nanoseconds(kRateNum * 1000000000ull / kRateDen);
	int frame_index = 0;
	while (std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds) && session.streaming()) {
		auto now = std::chrono::steady_clock::now();
		if (now >= next_video) {
			auto picture = color_bars(negotiated.config.width, negotiated.config.height, frame_index);
			if (sync_test) {
				bool flash = sync_pulse((double)frame_index * kRateNum / kRateDen);
				size_t luma = (size_t)negotiated.config.width * negotiated.config.height;
				std::fill(picture.begin(), picture.begin() + (std::ptrdiff_t)luma, flash ? 235 : 16);
				std::fill(picture.begin() + (std::ptrdiff_t)luma, picture.end(), 128);
			}
			airplay::RawNv12 raw;
			raw.data = picture.data();
			raw.size = picture.size();
			raw.width = negotiated.config.width;
			raw.height = negotiated.config.height;
			raw.timestamp_ns = (uint64_t)frame_index * kRateNum * 1000000000ull / kRateDen;
			raw.force_keyframe = frame_index == 0;
			std::string frame_error;
			video.encode(
				raw,
				[&](airplay::EncodedAu &&au) {
					airplay::VideoAu packet;
					packet.data = std::move(au.data);
					packet.codec_config = std::move(au.codec_config);
					packet.sample_description = std::move(au.sample_description);
					packet.keyframe = au.keyframe;
					packet.width = au.width;
					packet.height = au.height;
					packet.encode_ns = au.encode_ns;
					session.submit_video(std::move(packet));
				},
				frame_error);
			frame_index++;
			next_video += video_step;
		}
		if (negotiated.audio_negotiated && now >= next_audio) {
			std::vector<float> pcm(spf * 2);
			for (uint32_t i = 0; i < spf; i++) {
				float sample;
				if (sync_test) {
					/* A 1 kHz beep in step with the white flash. */
					sample = sync_pulse((double)sample_index++ / rate) ? 0.3f * std::sin((float)tone) : 0.f;
					tone += 2.0 * 3.141592653589793 * 1000.0 / rate;
				} else {
					sample = 0.2f * std::sin((float)tone);
					tone += 2.0 * 3.141592653589793 * 440.0 / rate;
				}
				pcm[(size_t)i * 2] = sample;
				pcm[(size_t)i * 2 + 1] = sample;
			}
			std::vector<airplay::AacPacket> encoded;
			audio.push(pcm.data(), spf, encoded);
			for (const airplay::AacPacket &packet : encoded)
				session.submit_audio(airplay::build_rtp_audio(seq++, packet.rtp_timestamp, ssrc, packet.data.data(),
									      packet.data.size(), packet.marker));
			next_audio += audio_step;
		}
		auto wake = next_audio < next_video ? next_audio : next_video;
		if (wake > now)
			std::this_thread::sleep_until(wake);
	}
	video.stop();
	audio.stop();
	session.stop();
	std::fprintf(stderr, "Stopped after %d frames.\n", frame_index);
	return 0;
}
