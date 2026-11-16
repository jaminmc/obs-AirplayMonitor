/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QDialog>

#include <string>

class QCloseEvent;

class MonitorDialog : public QDialog {
public:
	explicit MonitorDialog(QWidget *parent = nullptr);
	~MonitorDialog() override;

	void show_and_raise();
	void on_scenes();
	void on_video_reset();
	void collect(std::string &device_id, bool &preview, bool &send_audio, int &bitrate_kbps) const;
	void restore(const std::string &device_id, bool preview, bool send_audio, int bitrate_kbps);
	void set_audio_offset(int ms);
	void select_encoder();

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	struct Impl;
	Impl *impl_;
};

void airplay_monitor_show();
void airplay_monitor_shutdown();
void airplay_monitor_on_scenes();
void airplay_monitor_on_video_reset();
void airplay_monitor_collect(std::string &device_id, bool &preview, bool &send_audio, int &bitrate_kbps);
void airplay_monitor_restore(const std::string &device_id, bool preview, bool send_audio, int bitrate_kbps);
std::string airplay_monitor_encoder();
void airplay_monitor_set_encoder(const std::string &id);
int airplay_monitor_audio_offset();
void airplay_monitor_set_audio_offset(int ms);
