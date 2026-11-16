/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "airplay/session.hpp"

#include "airplay/aead.hpp"
#include "airplay/crypto.hpp"
#include "airplay/mirror_packet.hpp"
#include "airplay/ntp.hpp"
#include "airplay/pairing.hpp"
#include "airplay/rtp_audio.hpp"
#include "airplay/net.hpp"
#include "airplay/socket.hpp"

#include <memory>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace airplay {
namespace {

uint64_t mono_ns()
{
	auto now = std::chrono::steady_clock::now().time_since_epoch();
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

void store_be32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

} /* namespace */

struct Session::Impl {
	std::thread thread;
	std::atomic<bool> stop{false};
	std::atomic<bool> alive{false};
	std::atomic<bool> ready{false};
	std::atomic<uint64_t> dropped{0};
	std::mutex mutex;
	std::condition_variable cv;
	std::mutex pin_mu;
	std::condition_variable pin_cv;
	bool pin_waiting = false;
	bool pin_ready = false;
	std::string pin_value;
	std::deque<std::vector<uint8_t>> video_queue;
	struct QueuedAudio {
		std::vector<uint8_t> rtp;
		uint64_t capture_ns = 0;
	};
	std::deque<QueuedAudio> audio_queue;
	std::atomic<int> audio_offset_ms{0};
	HandshakeResult result;
	StatusFn on_status;
	ReadyFn on_ready;
	uint64_t start_ns = 0;
	bool saw_codec = false;
	std::vector<uint8_t> last_codec;
};

Session::Session() : impl_(new Impl) {}

Session::~Session()
{
	stop();
	delete impl_;
}

bool Session::running() const
{
	return impl_->alive.load();
}

bool Session::streaming() const
{
	return impl_->ready.load();
}

uint64_t Session::dropped() const
{
	return impl_->dropped.load();
}

const HandshakeResult &Session::result() const
{
	return impl_->result;
}

void Session::stop()
{
	impl_->stop.store(true);
	impl_->cv.notify_all();
	impl_->pin_cv.notify_all();
	if (impl_->thread.joinable())
		impl_->thread.join();
	impl_->alive.store(false);
	impl_->ready.store(false);
}

void Session::submit_video(VideoAu au)
{
	if (!impl_->ready.load() || impl_->stop.load())
		return;
	double seconds = au.session_seconds;
	if (seconds <= 0 && impl_->start_ns) {
		uint64_t now = mono_ns();
		if (now > impl_->start_ns)
			seconds = (double)(now - impl_->start_ns) / 1e9;
	}
	double offset = std::clamp((double)au.encode_ns / 1e9, 0.0, 0.05);
	uint64_t ntp = ntp_from_seconds(seconds + offset);
	bool hevc = impl_->result.config.codec == Codec::Hevc;
	/* Codec-config bookkeeping is touched from the encoder callback. The queue
	 * lock keeps that ordered with the network thread. */
	bool send_codec = false;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (!au.codec_config.empty() && au.codec_config != impl_->last_codec) {
			impl_->last_codec = au.codec_config;
			impl_->saw_codec = true;
			send_codec = true;
		} else if (!impl_->saw_codec && au.keyframe) {
			impl_->saw_codec = true;
		}
	}
	std::vector<std::vector<uint8_t>> packets;
	if (send_codec) {
		/* Receivers read H.264 settings as a bare avcC, and HEVC settings only
		   inside a QuickTime ImageDescription. */
		const std::vector<uint8_t> &codec = hevc && !au.sample_description.empty() ? au.sample_description
											   : au.codec_config;
		packets.push_back(build_mirror_packet(MirrorPayloadType::Codec, ntp, au.width, au.height, codec.data(),
						      codec.size(), false, hevc));
	}
	std::vector<uint8_t> payload = std::move(au.data);
	packets.push_back(build_mirror_packet(MirrorPayloadType::Video, ntp, au.width, au.height, payload.data(),
					      payload.size(), au.keyframe, hevc));
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		for (auto &packet : packets) {
			if (impl_->video_queue.size() >= 4) {
				auto drop = impl_->video_queue.begin();
				while (drop != impl_->video_queue.end() && drop->size() >= 6 && (*drop)[4] == 1 &&
				       (*drop)[5] == 0)
					++drop;
				if (drop == impl_->video_queue.end())
					drop = impl_->video_queue.begin();
				impl_->video_queue.erase(drop);
				impl_->dropped.fetch_add(1);
			}
			impl_->video_queue.push_back(std::move(packet));
		}
	}
	impl_->cv.notify_all();
}

void Session::set_audio_offset_ms(int ms)
{
	/* Audio arrives about 40 ms after capture and the sync allows 170 ms, so
	   playing more than 100 ms earlier would land packets after their time. */
	impl_->audio_offset_ms.store(std::clamp(ms, -100, 500));
}

void Session::submit_audio(std::vector<uint8_t> rtp, uint64_t capture_ns)
{
	if (!impl_->ready.load() || !impl_->result.audio_negotiated)
		return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->audio_queue.size() >= 64)
			impl_->audio_queue.pop_front();
		impl_->audio_queue.push_back({std::move(rtp), capture_ns});
	}
	impl_->cv.notify_all();
}

bool Session::waiting_for_pin() const
{
	std::lock_guard<std::mutex> lock(impl_->pin_mu);
	return impl_->pin_waiting;
}

void Session::submit_pin(std::string pin)
{
	std::lock_guard<std::mutex> lock(impl_->pin_mu);
	if (!impl_->pin_waiting)
		return;
	impl_->pin_value = std::move(pin);
	impl_->pin_ready = true;
	impl_->pin_cv.notify_all();
}

void Session::start(const HandshakeInput &input, StatusFn on_status, ReadyFn on_ready)
{
	stop();
	impl_->stop.store(false);
	{
		std::lock_guard<std::mutex> lock(impl_->pin_mu);
		impl_->pin_waiting = false;
		impl_->pin_ready = false;
		impl_->pin_value.clear();
	}
	impl_->ready.store(false);
	impl_->dropped.store(0);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->audio_queue.clear();
		impl_->video_queue.clear();
	}
	impl_->result = {};
	impl_->on_status = std::move(on_status);
	impl_->on_ready = std::move(on_ready);
	impl_->saw_codec = false;
	impl_->last_codec.clear();
	impl_->alive.store(true);
	HandshakeInput copy = input;
	impl_->thread = std::thread([this, copy]() {
		auto status = [&](const std::string &text) {
			if (impl_->on_status)
				impl_->on_status(text);
		};
		status("Connecting");
		UdpClock clock;
		UdpClock audio_out;
		UdpClock sync_out;
		std::string error;
		/* RTP and sync sockets are bound before SETUP so the audio stream can name them. */
		if (!clock.bind_any(copy.receiver.family, error) || !audio_out.bind_any(copy.receiver.family, error) ||
		    !sync_out.bind_any(copy.receiver.family, error)) {
			status(error);
			impl_->alive.store(false);
			return;
		}
		TcpTransport control;
		if (!control.connect_to(copy.receiver.address, copy.receiver.port, copy.receiver.family, 20000,
					error)) {
			status(error);
			impl_->alive.store(false);
			return;
		}
		HandshakeInput local = copy;
		local.timing_port = clock.port();
		local.audio_data_port = audio_out.port();
		local.audio_sync_port = sync_out.port();
		local.local_address = control.local_address();
		local.request_pin = [this]() {
			{
				std::lock_guard<std::mutex> lock(impl_->pin_mu);
				impl_->pin_waiting = true;
				impl_->pin_ready = false;
				impl_->pin_value.clear();
			}
			if (impl_->on_status)
				impl_->on_status("The TV is showing a pairing code. Type it and press Connect.");
			std::unique_lock<std::mutex> lock(impl_->pin_mu);
			impl_->pin_cv.wait_for(lock, std::chrono::minutes(3),
					       [&]() { return impl_->pin_ready || impl_->stop.load(); });
			impl_->pin_waiting = false;
			if (!impl_->pin_ready)
				return std::string();
			return impl_->pin_value;
		};
		impl_->start_ns = mono_ns();
		auto session_seconds = [&]() {
			uint64_t now = mono_ns();
			if (now < impl_->start_ns)
				return 0.0;
			return (double)(now - impl_->start_ns) / 1e9;
		};
		/* The timing replies and audio sync use uptime, the clock a Mac mirror sends.
		   Video packets use the session clock. */
		auto uptime_seconds = []() {
			return (double)mono_ns() / 1e9;
		};
		TcpTransport video;
		TcpTransport event_channel;
		local.prepare_video = [&](int port, std::string &prep_error) {
			return video.connect_to(copy.receiver.address, (uint16_t)port, copy.receiver.family, 5000,
						prep_error);
		};
		local.prepare_event = [&](int port, std::string &prep_error) {
			return event_channel.connect_to(copy.receiver.address, (uint16_t)port, copy.receiver.family,
							5000, prep_error);
		};
		local.on_receiver_timing = [&](int port) {
			if (copy.receiver.family == AF_INET)
				clock.probe_receiver(copy.receiver.address, (uint16_t)port,
						     ntp_from_seconds(uptime_seconds()));
		};
		std::atomic<bool> clock_stop{false};
		std::thread clock_thread([&]() {
			while (!clock_stop.load()) {
				clock.pump(ntp_from_seconds(uptime_seconds()));
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		});
		struct JoinClock {
			std::atomic<bool> &stop;
			std::thread &thread;
			~JoinClock()
			{
				stop.store(true);
				if (thread.joinable())
					thread.join();
			}
		} join_clock{clock_stop, clock_thread};
		std::unique_ptr<SealedControl> seal;
		HandshakeResult result = handshake(control, local, &seal);
		impl_->result = result;
		if (!result.ok) {
			status(result.error.empty() ? "Could not start the mirror" : result.error);
			impl_->alive.store(false);
			return;
		}
		/* RECORD starts a receiver timer of about a minute. POST /feedback on this
		 * same control connection, still sealed when pairing was used, resets it.
		 */
		ByteTransport *feedback_io = seal ? static_cast<ByteTransport *>(seal.get())
						  : static_cast<ByteTransport *>(&control);
		std::atomic<bool> feedback_stop{false};
		std::thread feedback_thread([feedback_io, cseq = result.next_cseq > 0 ? result.next_cseq : 1,
					     &feedback_stop]() { pump_feedback(*feedback_io, cseq, feedback_stop); });
		struct JoinFeedback {
			std::atomic<bool> &stop;
			std::thread &thread;
			~JoinFeedback()
			{
				stop.store(true);
				if (thread.joinable())
					thread.join();
			}
		} join_feedback{feedback_stop, feedback_thread};
		std::unique_ptr<SealedControl> event_seal;
		ByteTransport *event_io = nullptr;
		if (event_channel.is_open()) {
			bool cleartext = false;
			uint8_t peek[4] = {};
			int peeked = event_channel.peek(peek, sizeof peek);
			if (peeked >= 2 && peek[0] >= 'A' && peek[1] >= 'A')
				cleartext = true;
			if (!cleartext && result.event_read_key.size() == 32 && result.event_write_key.size() == 32) {
				event_seal = std::make_unique<SealedControl>(
					event_channel, result.event_write_key.data(), result.event_read_key.data());
				event_io = event_seal.get();
			} else {
				event_io = &event_channel;
			}
		}
		std::atomic<bool> event_stop{false};
		std::thread event_thread;
		if (event_io) {
			event_thread = std::thread([event_io, &event_stop]() { pump_events(*event_io, event_stop); });
		}
		struct JoinEvent {
			std::atomic<bool> &stop;
			std::thread &thread;
			~JoinEvent()
			{
				stop.store(true);
				if (thread.joinable())
					thread.join();
			}
		} join_event{event_stop, event_thread};
		if (result.video_port <= 0) {
			status("The receiver did not open a video port");
			impl_->alive.store(false);
			return;
		}
		if (!video.is_open() && !video.connect_to(copy.receiver.address, (uint16_t)result.video_port,
							  copy.receiver.family, 5000, error)) {
			status(error.empty() ? "Could not open the video stream" : error);
			impl_->alive.store(false);
			return;
		}
		video.prefer_low_latency();
		bool audio_open = result.audio_negotiated && result.audio_port > 0;
		AesCtr aes;
		if (result.use_chacha) {
			if (result.chacha_key.size() != 32) {
				status("Could not start encrypted video");
				impl_->alive.store(false);
				return;
			}
		} else if (result.encrypt) {
			std::string crypto_error;
			if (!aes.set_key(result.aes_key.data(), result.aes_iv.data(), crypto_error)) {
				status(crypto_error);
				impl_->alive.store(false);
				return;
			}
		}
		uint64_t video_nonce = 0;
		uint64_t audio_nonce = 1;
		/* The first RTP time has to sit ahead of the sync lead, or the timestamp wraps. */
		const uint32_t rtp_origin = result.audio_sample_rate;
		bool audio_sync_sent = false;
		uint64_t last_audio_sync = 0;
		AudioSyncClock sync_clock;
		std::deque<std::vector<uint8_t>> recent_rtp;
		auto send_rtp = [&](std::vector<uint8_t> audio_packet, uint64_t capture_ns) {
			if (!audio_open || audio_packet.size() < 12)
				return;
			uint32_t rtp_time = ((uint32_t)audio_packet[4] << 24) | ((uint32_t)audio_packet[5] << 16) |
					    ((uint32_t)audio_packet[6] << 8) | audio_packet[7];
			/* A timestamp of zero is logged as an empty packet and never plays. */
			rtp_time += rtp_origin;
			store_be32(audio_packet.data() + 4, rtp_time);
			/* A Mac mirror leaves the marker clear and the SSRC at zero. */
			audio_packet[1] = 96;
			store_be32(audio_packet.data() + 8, 0);
			if (result.audio_chacha_key.size() == 32) {
				/* A Mac mirror counts its audio nonces 1, 3, 5. */
				uint64_t nonce = audio_nonce;
				audio_nonce += 2;
				std::vector<uint8_t> sealed_payload;
				/* The receiver swaps timestamp and SSRC in the datagram, then swaps
				   those eight bytes again for the ChaCha extra data. The extra
				   data is therefore the original big-endian header bytes. */
				if (!aead_seal(result.audio_chacha_key.data(), nonce, audio_packet.data() + 4, 8,
					       audio_packet.data() + 12, audio_packet.size() - 12, sealed_payload))
					return;
				audio_packet.resize(12);
				audio_packet.insert(audio_packet.end(), sealed_payload.begin(), sealed_payload.end());
				for (int i = 0; i < 8; i++)
					audio_packet.push_back((uint8_t)(nonce >> (8 * i)));
			}
			uint64_t now_ns = mono_ns();
			if (result.audio_control_port > 0 &&
			    (!audio_sync_sent || now_ns - last_audio_sync > 1000000000ull)) {
				constexpr uint64_t kEpoch1900 = 2208988800ull;
				uint64_t ntp = ntp_from_seconds(uptime_seconds()) + (kEpoch1900 << 32);
				uint32_t rate = result.audio_sample_rate;
				/* 170 ms, the lead a Mac mirror uses. */
				uint32_t lead = rate * 17 / 100;
				uint32_t due = sync_clock.due(rtp_time, capture_ns, now_ns, rate);
				due -= (uint32_t)((int64_t)impl_->audio_offset_ms.load() * rate / 1000);
				auto sync = classic_sync_packet(due - lead, due, ntp, !audio_sync_sent);
				/* Sync leaves from the port the audio SETUP named, not the timing port. */
				sync_out.send_to(copy.receiver.address, (uint16_t)result.audio_control_port,
						 copy.receiver.family, sync.data(), sync.size());
				audio_sync_sent = true;
				last_audio_sync = now_ns;
			}
			/* UDP has no resend, so a Mac mirror sends each frame three times:
			   every datagram repeats the previous two frames, then the new one. */
			recent_rtp.push_back(std::move(audio_packet));
			if (recent_rtp.size() > 3)
				recent_rtp.pop_front();
			for (const auto &frame : recent_rtp)
				audio_out.send_to(copy.receiver.address, (uint16_t)result.audio_port,
						  copy.receiver.family, frame.data(), frame.size());
		};
		impl_->ready.store(true);
		status(result.status);
		if (impl_->on_ready)
			impl_->on_ready(result);

		/* Audio has its own thread. A video write can block longer than the
		   receiver's audio lead, and audio queued behind it then plays late. */
		std::atomic<bool> audio_stop{false};
		std::thread audio_thread([&]() {
			while (!impl_->stop.load() && !audio_stop.load()) {
				std::vector<Impl::QueuedAudio> audio_packets;
				{
					std::unique_lock<std::mutex> lock(impl_->mutex);
					impl_->cv.wait_for(lock, std::chrono::milliseconds(20), [&]() {
						return impl_->stop.load() || audio_stop.load() ||
						       !impl_->audio_queue.empty();
					});
					while (!impl_->audio_queue.empty() && audio_packets.size() < 32) {
						audio_packets.push_back(std::move(impl_->audio_queue.front()));
						impl_->audio_queue.pop_front();
					}
				}
				for (Impl::QueuedAudio &audio_packet : audio_packets)
					send_rtp(std::move(audio_packet.rtp), audio_packet.capture_ns);
			}
		});
		struct JoinAudio {
			std::atomic<bool> &stop;
			std::thread &thread;
			std::condition_variable &cv;
			~JoinAudio()
			{
				stop.store(true);
				cv.notify_all();
				if (thread.joinable())
					thread.join();
			}
		} join_audio{audio_stop, audio_thread, impl_->cv};

		uint64_t last_heartbeat = mono_ns();
		bool report_disconnect = true;
		while (!impl_->stop.load()) {
			std::vector<uint8_t> video_packet;
			{
				std::unique_lock<std::mutex> lock(impl_->mutex);
				impl_->cv.wait_for(lock, std::chrono::milliseconds(20),
						   [&]() { return impl_->stop.load() || !impl_->video_queue.empty(); });
				if (!impl_->video_queue.empty()) {
					video_packet = std::move(impl_->video_queue.front());
					impl_->video_queue.pop_front();
				}
			}
			if (!video_packet.empty()) {
				if (video_packet.size() > kMirrorHeaderSize) {
					/* Byte 5 is the IDR flag, so the kind is byte 4 alone. */
					uint8_t kind = video_packet[4];
					if (kind == (uint8_t)MirrorPayloadType::Video && result.use_chacha) {
						size_t plain_len = video_packet.size() - kMirrorHeaderSize;
						uint32_t wire_len = (uint32_t)plain_len + 16;
						video_packet[0] = (uint8_t)wire_len;
						video_packet[1] = (uint8_t)(wire_len >> 8);
						video_packet[2] = (uint8_t)(wire_len >> 16);
						video_packet[3] = (uint8_t)(wire_len >> 24);
						std::vector<uint8_t> sealed_payload;
						if (!aead_seal(result.chacha_key.data(), video_nonce++,
							       video_packet.data(), kMirrorHeaderSize,
							       video_packet.data() + kMirrorHeaderSize, plain_len,
							       sealed_payload)) {
							status("Could not encrypt a video frame");
							report_disconnect = false;
							break;
						}
						video_packet.resize(kMirrorHeaderSize);
						video_packet.insert(video_packet.end(), sealed_payload.begin(),
								    sealed_payload.end());
					} else if (kind == (uint8_t)MirrorPayloadType::Video && result.encrypt) {
						std::string crypto_error;
						if (!aes.process(video_packet.data() + kMirrorHeaderSize,
								 video_packet.size() - kMirrorHeaderSize,
								 crypto_error)) {
							status(crypto_error.empty() ? "Could not encrypt a video frame"
										    : crypto_error);
							report_disconnect = false;
							break;
						}
					}
				}
				if (!video.write_all(video_packet.data(), video_packet.size())) {
					if (!impl_->stop.load()) {
						status("The video connection closed");
						report_disconnect = false;
					}
					break;
				}
				last_heartbeat = mono_ns();
			}
			uint64_t now = mono_ns();
			if (now - last_heartbeat > 1000000000ull) {
				auto beat = build_mirror_packet(MirrorPayloadType::Heartbeat,
								ntp_from_seconds(session_seconds()),
								result.config.width, result.config.height, nullptr, 0);
				if (!video.write_all(beat.data(), beat.size())) {
					if (!impl_->stop.load()) {
						status("The video connection closed");
						report_disconnect = false;
					}
					break;
				}
				last_heartbeat = now;
			}
		}
		impl_->ready.store(false);
		impl_->alive.store(false);
		if (report_disconnect)
			status("Disconnected");
	});
}

} /* namespace airplay */
