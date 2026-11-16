/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "encode/obs_video.hpp"
#include "ui/monitor_dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

bool menu_added = false;

void add_tools_menu()
{
	if (menu_added)
		return;
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("MenuTitle")));
	if (!action)
		return;
	action->setMenuRole(QAction::NoRole);
	QObject::connect(action, &QAction::triggered, []() { airplay_monitor_show(); });
	menu_added = true;
}

void save_callback(obs_data_t *save_data, bool saving, void *)
{
	if (saving) {
		std::string device_id;
		bool preview = false;
		bool send_audio = false;
		int bitrate = 0;
		airplay_monitor_collect(device_id, preview, send_audio, bitrate);
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "device_id", device_id.c_str());
		obs_data_set_bool(settings, "preview", preview);
		obs_data_set_bool(settings, "send_audio", send_audio);
		obs_data_set_int(settings, "bitrate_kbps", bitrate);
		obs_data_set_int(settings, "audio_offset_ms", airplay_monitor_audio_offset());
		obs_data_set_string(settings, "video_encoder", airplay_monitor_encoder().c_str());
		obs_data_set_obj(save_data, "airplay_monitor", settings);
		obs_data_release(settings);
		return;
	}
	obs_data_t *settings = obs_data_get_obj(save_data, "airplay_monitor");
	if (!settings)
		return;
	airplay_monitor_restore(obs_data_get_string(settings, "device_id"), obs_data_get_bool(settings, "preview"),
				obs_data_get_bool(settings, "send_audio"),
				(int)obs_data_get_int(settings, "bitrate_kbps"));
	airplay_monitor_set_audio_offset((int)obs_data_get_int(settings, "audio_offset_ms"));
	airplay_monitor_set_encoder(obs_data_get_string(settings, "video_encoder"));
	obs_data_release(settings);
}

void frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		add_tools_menu();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		airplay_monitor_shutdown();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		airplay_monitor_on_scenes();
		break;
	default:
		break;
	}
}

} /* namespace */

bool obs_module_load(void)
{
	airplay::ObsVideoEncoder::register_output();
	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_frontend_add_save_callback(save_callback, nullptr);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	airplay_monitor_shutdown();
	obs_frontend_remove_save_callback(save_callback, nullptr);
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
