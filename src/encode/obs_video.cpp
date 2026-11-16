/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "encode/obs_video.hpp"

#include "encode/bitstream.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace airplay {
namespace {

const char *kOutputId = "airplay_monitor_encoded";

bool is_software(const char *id, const char *name)
{
	return std::strcmp(id, "obs_x264") == 0 || std::strcmp(id, "ffmpeg_openh264") == 0 ||
	       (name && std::strstr(name, "Software"));
}

/* Settings are written only where the encoder's defaults already have the key, with the same type,
   because each encoder names and types its options differently. */
class EncoderSettings {
public:
	explicit EncoderSettings(const char *id) : defaults_(obs_encoder_defaults(id)), settings_(obs_data_create()) {}
	~EncoderSettings()
	{
		obs_data_release(defaults_);
		obs_data_release(settings_);
	}

	void set_int(const char *key, long long value)
	{
		if (type_of(key) == OBS_DATA_NUMBER)
			obs_data_set_int(settings_, key, value);
	}
	void set_bool(const char *key, bool value)
	{
		if (type_of(key) == OBS_DATA_BOOLEAN)
			obs_data_set_bool(settings_, key, value);
	}
	void set_string(const char *key, const char *value)
	{
		if (type_of(key) == OBS_DATA_STRING)
			obs_data_set_string(settings_, key, value);
	}
	obs_data_t *data() const { return settings_; }

private:
	enum obs_data_type type_of(const char *key) const
	{
		obs_data_item_t *item = obs_data_item_byname(defaults_, key);
		if (!item)
			return OBS_DATA_NULL;
		enum obs_data_type type = obs_data_item_gettype(item);
		obs_data_item_release(&item);
		return type;
	}

	obs_data_t *defaults_;
	obs_data_t *settings_;
};

uint32_t canvas_fps()
{
	obs_video_info info;
	if (!obs_get_video_info(&info) || info.fps_den == 0)
		return 30;
	uint32_t fps = (info.fps_num + info.fps_den / 2) / info.fps_den;
	return fps ? fps : 30;
}

} /* namespace */

std::vector<VideoEncoderChoice> video_encoder_choices()
{
	std::vector<VideoEncoderChoice> hardware;
	std::vector<VideoEncoderChoice> software;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || (std::strcmp(codec, "h264") != 0 && std::strcmp(codec, "hevc") != 0))
			continue;
		VideoEncoderChoice choice;
		choice.id = id;
		const char *name = obs_encoder_get_display_name(id);
		choice.name = name ? name : id;
		choice.codec = std::strcmp(codec, "hevc") == 0 ? Codec::Hevc : Codec::H264;
		choice.hardware = !is_software(id, name);
		(choice.hardware ? hardware : software).push_back(std::move(choice));
	}
	hardware.insert(hardware.end(), software.begin(), software.end());
	return hardware;
}

std::vector<std::string> encoder_candidates(const std::string &preferred, Codec codec)
{
	std::vector<std::string> ids;
	for (const VideoEncoderChoice &choice : video_encoder_choices()) {
		if (choice.codec != codec)
			continue;
		if (choice.id == preferred)
			ids.insert(ids.begin(), choice.id);
		else
			ids.push_back(choice.id);
	}
	return ids;
}

bool encoder_allows_hevc(const std::string &preferred)
{
	for (const VideoEncoderChoice &choice : video_encoder_choices()) {
		if (choice.codec == Codec::Hevc && (preferred.empty() || choice.id == preferred))
			return true;
	}
	return false;
}

struct ObsVideoEncoder::Impl {
	obs_view_t *view = nullptr;
	obs_encoder_t *encoder = nullptr;
	obs_output_t *output = nullptr;
	std::string encoder_id;
	EncodeConfig config;
	std::atomic<uint64_t> frames{0};

	std::mutex mutex;
	Callback callback;
	ParameterSets sets;
	std::vector<uint8_t> sent_config;
	bool started = false;

	void on_packet(struct encoder_packet *packet)
	{
		if (!packet || packet->type != OBS_ENCODER_VIDEO || !packet->data)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		if (!callback)
			return;
		EncodedAu au;
		if (!annexb_to_length_prefixed(config.codec, packet->data, packet->size, au.data, sets) ||
		    au.data.empty())
			return;
		au.keyframe = packet->keyframe;
		au.width = config.width;
		au.height = config.height;
		if (au.keyframe) {
			if (sets.sps.empty() || sets.pps.empty() || (config.codec == Codec::Hevc && sets.vps.empty()))
				take_extra_data();
			std::vector<uint8_t> codec_config;
			bool built = config.codec == Codec::Hevc ? build_hvcc(sets, codec_config)
								 : build_avcc(sets, codec_config);
			if (built && codec_config != sent_config) {
				sent_config = codec_config;
				au.sample_description =
					image_description(config.codec, config.width, config.height, codec_config);
				au.codec_config = std::move(codec_config);
			}
			if (built)
				started = true;
		}
		/* The receiver cannot decode anything before the first parameter sets. */
		if (!started)
			return;
		uint64_t now_us = os_gettime_ns() / 1000;
		if (packet->sys_dts_usec > 0 && now_us > (uint64_t)packet->sys_dts_usec)
			au.encode_ns = (now_us - (uint64_t)packet->sys_dts_usec) * 1000;
		frames.fetch_add(1);
		callback(std::move(au));
	}

	void take_extra_data()
	{
		uint8_t *extra = nullptr;
		size_t size = 0;
		if (!encoder || !obs_encoder_get_extra_data(encoder, &extra, &size) || !extra || !size)
			return;
		std::vector<uint8_t> ignored;
		annexb_to_length_prefixed(config.codec, extra, size, ignored, sets);
	}

	struct Binding {
		obs_output_t *output = nullptr;
		std::atomic<Impl *> impl{nullptr};
	};

	static const char *get_name(void *) { return "AirPlay Monitor"; }

	/* obs_output_create calls create on the caller's thread. libobs has no getter for the data it
	   returns, so start() collects it from here. */
	static Binding *&created()
	{
		static Binding *binding = nullptr;
		return binding;
	}

	static void *create(obs_data_t *, obs_output_t *output)
	{
		auto *binding = new Binding;
		binding->output = output;
		created() = binding;
		return binding;
	}

	static void destroy(void *data) { delete static_cast<Binding *>(data); }

	static bool start_output(void *data)
	{
		obs_output_t *output = static_cast<Binding *>(data)->output;
		if (!obs_output_can_begin_data_capture(output, 0) || !obs_output_initialize_encoders(output, 0))
			return false;
		return obs_output_begin_data_capture(output, 0);
	}

	static void stop_output(void *data, uint64_t)
	{
		obs_output_end_data_capture(static_cast<Binding *>(data)->output);
	}

	static void packet(void *data, struct encoder_packet *packet)
	{
		if (Impl *self = static_cast<Binding *>(data)->impl.load())
			self->on_packet(packet);
	}

	void release()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			callback = nullptr;
		}
		if (output) {
			obs_output_force_stop(output);
			obs_output_release(output);
			output = nullptr;
		}
		if (encoder) {
			obs_encoder_release(encoder);
			encoder = nullptr;
		}
		if (view) {
			obs_view_set_source(view, 0, nullptr);
			obs_view_remove(view);
			obs_view_destroy(view);
			view = nullptr;
		}
	}
};

void ObsVideoEncoder::register_output()
{
	static bool once = false;
	if (once)
		return;
	obs_output_info info{};
	info.id = kOutputId;
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.encoded_video_codecs = "h264;hevc";
	info.get_name = Impl::get_name;
	info.create = Impl::create;
	info.destroy = Impl::destroy;
	info.start = Impl::start_output;
	info.stop = Impl::stop_output;
	info.encoded_packet = Impl::packet;
	obs_register_output(&info);
	once = true;
}

ObsVideoEncoder::ObsVideoEncoder() : impl_(new Impl) {}

ObsVideoEncoder::~ObsVideoEncoder()
{
	stop();
	delete impl_;
}

bool ObsVideoEncoder::start(const std::string &encoder_id, const EncodeConfig &config, bool preview, Callback callback,
			    std::string &error)
{
	stop();
	obs_video_info info;
	if (!obs_get_video_info(&info) || !obs_get_video()) {
		error = "OBS video is not running";
		return false;
	}
	impl_->encoder_id = encoder_id;
	impl_->config = config;
	impl_->frames.store(0);
	impl_->sets = {};
	impl_->sent_config.clear();
	impl_->started = false;

	video_t *video = obs_get_video();
	if (preview) {
		impl_->view = obs_view_create();
		obs_video_info view_info = info;
		view_info.output_width = config.width;
		view_info.output_height = config.height;
		video = impl_->view ? obs_view_add2(impl_->view, &view_info) : nullptr;
		if (!video) {
			impl_->release();
			error = "Could not open the Preview view";
			return false;
		}
		update_preview_scene();
	}

	const bool hevc = config.codec == Codec::Hevc;
	EncoderSettings settings(encoder_id.c_str());
	long long kbps = std::max<long long>(1, config.bitrate / 1000);
	settings.set_int("bitrate", kbps);
	settings.set_int("max_bitrate", kbps);
	settings.set_int("keyint_sec", 2);
	settings.set_int("bf", 0);
	settings.set_int("bframes", 0);
	settings.set_bool("bframes", false);
	settings.set_bool("repeat_headers", true);
	settings.set_bool("lookahead", false);
	settings.set_string("profile", hevc ? "main" : "high");
	if (encoder_id == "obs_x264")
		settings.set_string("tune", "zerolatency");
	if (encoder_id.find("nvenc") != std::string::npos)
		settings.set_string("tune", "ll");

	impl_->encoder =
		obs_video_encoder_create(encoder_id.c_str(), "AirPlay Monitor Video", settings.data(), nullptr);
	if (!impl_->encoder) {
		impl_->release();
		error = "Could not create the " + encoder_name() + " encoder";
		return false;
	}
	if (!preview && (config.width != info.output_width || config.height != info.output_height)) {
		obs_encoder_set_scaled_size(impl_->encoder, config.width, config.height);
		obs_encoder_set_gpu_scale_type(impl_->encoder, OBS_SCALE_BICUBIC);
	}
	uint32_t fps = canvas_fps();
	uint32_t divisor = config.fps ? (fps + config.fps / 2) / config.fps : 1;
	if (divisor > 1)
		obs_encoder_set_frame_rate_divisor(impl_->encoder, divisor);
	obs_encoder_set_video(impl_->encoder, video);

	Impl::created() = nullptr;
	impl_->output = obs_output_create(kOutputId, "AirPlay Monitor", nullptr, nullptr);
	Impl::Binding *binding = impl_->output ? Impl::created() : nullptr;
	Impl::created() = nullptr;
	if (!binding) {
		impl_->release();
		error = "Could not create the AirPlay output";
		return false;
	}
	binding->impl.store(impl_);
	obs_output_set_video_encoder(impl_->output, impl_->encoder);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->callback = std::move(callback);
	}
	if (!obs_output_start(impl_->output)) {
		const char *last = obs_output_get_last_error(impl_->output);
		error = encoder_name() + " did not start";
		if (last && *last)
			error += std::string(": ") + last;
		impl_->release();
		return false;
	}
	return true;
}

void ObsVideoEncoder::update_preview_scene()
{
	if (!impl_->view)
		return;
	obs_source_t *scene = nullptr;
	if (obs_frontend_preview_program_mode_active())
		scene = obs_frontend_get_current_preview_scene();
	if (!scene)
		scene = obs_frontend_get_current_scene();
	if (!scene)
		return;
	obs_view_set_source(impl_->view, 0, scene);
	obs_source_release(scene);
}

void ObsVideoEncoder::stop()
{
	impl_->release();
}

bool ObsVideoEncoder::running() const
{
	return impl_->output != nullptr;
}

std::string ObsVideoEncoder::encoder_name() const
{
	const char *name = impl_->encoder_id.empty() ? nullptr
						     : obs_encoder_get_display_name(impl_->encoder_id.c_str());
	return name ? name : impl_->encoder_id;
}

uint64_t ObsVideoEncoder::frames() const
{
	return impl_->frames.load();
}

uint64_t ObsVideoEncoder::dropped() const
{
	return impl_->output ? (uint64_t)std::max(0, obs_output_get_frames_dropped(impl_->output)) : 0;
}

} /* namespace airplay */
