/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/features.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace airplay {

/* Screen-audio RTP clock. At the dump's 44.1 kHz the audio slowly fell behind the picture. */
constexpr uint32_t kAudioRate = 48000;

class SealedControl;

class ByteTransport {
public:
	virtual ~ByteTransport() = default;
	virtual bool write_all(const uint8_t *data, size_t size) = 0;
	/* Positive byte count, 0 on timeout or close, negative on error. */
	virtual int read_some(uint8_t *data, size_t capacity, int timeout_ms) = 0;
};

struct HandshakeInput {
	ReceiverInfo receiver;
	bool send_audio = false;
	/* False when the chosen encoder makes only H.264. */
	bool allow_hevc = true;
	uint32_t src_width = 1920;
	uint32_t src_height = 1080;
	uint32_t src_fps = 60;
	uint32_t bitrate_override_bps = 0;
	uint16_t timing_port = 0;
	/* Local RTP and sync ports named in the audio SETUP. Zero keeps the timing port. */
	uint16_t audio_data_port = 0;
	uint16_t audio_sync_port = 0;
	/* Local address of the control connection. A PTP peer list names this. */
	std::string local_address;
	/* How long one control read waits. The receiver may pause a SETUP until its clock probe is answered. */
	/* The Roku receiver app can take about 10 seconds to answer the first request. */
	/* Covers the Mac approval prompt before pair-pin-start returns. */
	int response_timeout_ms = 20000;
	std::string pin;
	/* Called after the receiver has been asked to show a code. Empty means cancelled. */
	std::function<std::string()> request_pin;
	/* Called after stream SETUP and before RECORD, so the video port is open first. */
	std::function<bool(int video_port, std::string &error)> prepare_video;
	/* Optional event channel from a SETUP response. Empty means leave it unused. */
	std::function<bool(int event_port, std::string &error)> prepare_event;
	/* The receiver included its own timing port. The sender probes it. */
	std::function<void(int timing_port)> on_receiver_timing;
	/* A receiver with no FairPlay bits can still mirror after pairing. A 404 on
	 * /fp-setup is then ignored and the stream key comes from the pair-verify secret.
	 */
	bool continue_without_fairplay = false;
};

struct HandshakeResult {
	bool ok = false;
	std::string error;
	std::string status;
	EncodeConfig config;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	uint32_t max_fps = 0;
	int video_port = 0;
	int event_port = 0;
	int receiver_timing_port = 0;
	std::string setup_detail;
	bool audio_negotiated = false;
	int audio_port = 0;
	int audio_control_port = 0;
	/* One AAC-ELD frame per RTP datagram, over UDP. */
	uint32_t audio_sample_rate = kAudioRate;
	uint32_t audio_frame_samples = 480;
	/* Empty means the RTP payload stays in the clear. */
	std::vector<uint8_t> audio_chacha_key;
	bool encrypt = false;
	std::vector<uint8_t> aes_key;
	std::vector<uint8_t> aes_iv;
	bool use_chacha = false;
	std::vector<uint8_t> chacha_key;
	/* Event channel reverses the control keys. Read uses Events-Write, write uses Events-Read. */
	std::vector<uint8_t> event_read_key;
	std::vector<uint8_t> event_write_key;
	bool fairplay_advertised = false;
	/* Logical requests, including those later sealed on the control connection. */
	std::vector<std::string> requests;
	/* Next RTSP CSeq after RECORD. Feedback continues from here. */
	int next_cseq = 1;
};

/* When retain_seal is set, a pair-verified control connection stays encrypted for the caller. */
HandshakeResult handshake(ByteTransport &transport, const HandshakeInput &input,
			  std::unique_ptr<SealedControl> *retain_seal = nullptr);

/* Binary plist for the screen-audio SETUP, in the shape a Mac sends. An empty shk leaves
   the audio in the clear. control_port is the local port the sync packets leave from. */
bool audio_setup_body(uint64_t stream_id, const std::vector<uint8_t> &shk, int control_port, std::vector<uint8_t> &body,
		      std::string &error);

/* One POST /feedback. The receiver drops a mirror that goes about a minute without one. */
bool post_feedback(ByteTransport &transport, int &cseq, std::string &error);

/* Sends feedback immediately, then every 2 seconds, until stop is set or the control connection closes. */
void pump_feedback(ByteTransport &transport, int cseq, const std::atomic<bool> &stop);

/* Reads one event-channel request and answers RTSP/1.0 200. `pending` keeps a following request. */
bool answer_event(ByteTransport &transport, std::vector<uint8_t> &pending, const std::atomic<bool> *stop,
		  std::string &error);

/* Answers event requests until stop is set or the event connection fails. */
void pump_events(ByteTransport &transport, const std::atomic<bool> &stop);

} /* namespace airplay */
