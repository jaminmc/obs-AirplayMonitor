/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/monitor_dialog.hpp"

#include "airplay/discovery.hpp"
#include "airplay/rtp_audio.hpp"
#include "airplay/session.hpp"
#include "audio/aac_eld.hpp"
#include "encode/obs_video.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>

namespace {

struct Settings {
	std::string device_id;
	bool preview = false;
	bool send_audio = false;
	int bitrate_kbps = 0;
	int audio_offset_ms = 0;
	/* Empty picks an encoder automatically. */
	std::string encoder_id;
};

Settings g_settings;
MonitorDialog *g_dialog = nullptr;

uint32_t canvas_fps()
{
	obs_video_info info;
	if (!obs_get_video_info(&info) || info.fps_den == 0)
		return 30;
	uint32_t fps = (info.fps_num + info.fps_den / 2) / info.fps_den;
	return fps ? fps : 30;
}

} /* namespace */

struct MonitorDialog::Impl {
	airplay::Discovery discovery;
	airplay::Session session;
	airplay::ObsVideoEncoder video;
	airplay::AacEldEncoder audio;
	std::atomic<uint64_t> encode_ns{0};
	std::vector<airplay::DiscoveredReceiver> devices;
	airplay::EncodeConfig config;
	uint32_t src_width = 0;
	uint32_t src_height = 0;
	uint32_t src_fps = 30;
	uint32_t fps_num = 60;
	uint32_t fps_den = 1;
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	uint32_t max_fps = 0;
	uint64_t connect_gen = 0;
	uint32_t audio_ssrc = 0x41504d31;
	uint16_t audio_seq = 0;
	uint32_t audio_rate = 48000;
	/* steady_clock time of the first sample handed to the encoder. */
	std::atomic<uint64_t> audio_origin_ns{0};
	bool audio_on = false;
	bool streaming = false;

	QDialog *owner = nullptr;
	QListWidget *list = nullptr;
	QRadioButton *program = nullptr;
	QRadioButton *preview = nullptr;
	QCheckBox *audio_box = nullptr;
	QLabel *audio_note = nullptr;
	bool audio_available = true;
	QComboBox *encoder_box = nullptr;
	QSpinBox *bitrate = nullptr;
	QSpinBox *audio_offset = nullptr;
	QLineEdit *pin = nullptr;
	QLabel *status = nullptr;
	QLabel *stats = nullptr;
	QPushButton *connect = nullptr;
	QPushButton *disconnect = nullptr;

	void set_status(const QString &text) { status->setText(text); }

	void select_encoder()
	{
		int index = encoder_box->findData(QString::fromStdString(g_settings.encoder_id));
		QSignalBlocker block(encoder_box);
		encoder_box->setCurrentIndex(index < 0 ? 0 : index);
	}

	void refresh_audio_note()
	{
		bool show = preview->isChecked() && audio_box->isChecked();
		audio_note->setVisible(show);
	}

	void rebuild_list()
	{
		QString selected = g_settings.device_id.c_str();
		list->clear();
		int select_row = -1;
		for (size_t i = 0; i < devices.size(); i++) {
			const airplay::DiscoveredReceiver &device = devices[i];
			QString title = QString::fromStdString(device.name.empty() ? device.host : device.name);
			if (!device.model.empty())
				title += QString(" — ") + QString::fromStdString(device.model);
			if (!device.address.empty())
				title += QString(" — ") + QString::fromStdString(device.address);
			title += QString(" — ") + QString::fromUtf8(airplay::reject_text(device.reason));
			auto *item = new QListWidgetItem(title, list);
			item->setData(Qt::UserRole, (int)i);
			if (!device.device_id.empty() && device.device_id == selected.toStdString())
				select_row = (int)i;
		}
		if (select_row >= 0)
			list->setCurrentRow(select_row);
	}

	const airplay::DiscoveredReceiver *selected() const
	{
		auto *item = list->currentItem();
		if (!item)
			return nullptr;
		int index = item->data(Qt::UserRole).toInt();
		if (index < 0 || index >= (int)devices.size())
			return nullptr;
		return &devices[(size_t)index];
	}

	void stop_pipeline()
	{
		++connect_gen;
		video.stop();
		audio.stop();
		if (audio_on)
			obs_remove_raw_audio_callback(0, &Impl::on_audio, this);
		audio_on = false;
		session.stop();
		streaming = false;
		disconnect->setEnabled(false);
		connect->setEnabled(true);
	}

	void fail_pipeline(const QString &message)
	{
		stop_pipeline();
		set_status(message);
	}

	void deliver(airplay::EncodedAu &&au)
	{
		encode_ns.store(au.encode_ns);
		airplay::VideoAu frame;
		frame.data = std::move(au.data);
		frame.codec_config = std::move(au.codec_config);
		frame.sample_description = std::move(au.sample_description);
		frame.keyframe = au.keyframe;
		frame.width = au.width;
		frame.height = au.height;
		frame.encode_ns = au.encode_ns;
		session.submit_video(std::move(frame));
	}

	bool try_encoders(const airplay::EncodeConfig &wanted, std::string &error)
	{
		for (const std::string &id : airplay::encoder_candidates(g_settings.encoder_id, wanted.codec)) {
			if (video.start(id, wanted, preview->isChecked(),
					[this](airplay::EncodedAu &&au) { deliver(std::move(au)); }, error)) {
				config = wanted;
				return true;
			}
			blog(LOG_WARNING, "[obs-airplay-monitor] %s", error.c_str());
		}
		if (error.empty())
			error = std::string("OBS has no ") + airplay::codec_name(wanted.codec) + " encoder";
		return false;
	}

	bool start_video(airplay::EncodeConfig wanted, QString &error_text)
	{
		std::string error;
		if (try_encoders(wanted, error))
			return true;
		if (wanted.codec == airplay::Codec::Hevc) {
			uint32_t override_bps = bitrate->value() > 0 ? (uint32_t)bitrate->value() * 1000u : 0;
			wanted = airplay::choose_encode(src_width, src_height, src_fps, wanted.width, wanted.height, wanted.fps,
							false, override_bps);
			wanted.fell_back_from_hevc = true;
			if (try_encoders(wanted, error))
				return true;
		}
		error_text = QString::fromStdString(error);
		return false;
	}

	void ensure_audio()
	{
		if (audio_on || !session.result().audio_negotiated || !audio_box->isChecked())
			return;
		std::string error;
		uint32_t rate = session.result().audio_sample_rate;
		if (!rate)
			rate = 48000;
		if (!audio.start(rate, error))
			return;
		audio_rate = rate;
		audio_origin_ns.store(0);
		audio_convert_info conv{};
		conv.samples_per_sec = rate;
		conv.format = AUDIO_FORMAT_FLOAT_PLANAR;
		conv.speakers = SPEAKERS_STEREO;
		obs_add_raw_audio_callback(0, &conv, &Impl::on_audio, this);
		audio_on = true;
	}

	bool retarget_video()
	{
		QString error;
		if (start_video(config, error))
			return true;
		set_status(error);
		return false;
	}

	void begin_streaming()
	{
		ensure_audio();
		streaming = true;
		connect->setEnabled(false);
		disconnect->setEnabled(true);
	}

	static void on_audio(void *param, size_t, struct audio_data *data)
	{
		auto *self = static_cast<Impl *>(param);
		if (!self->audio_on || !data || !data->data[0] || !data->data[1] || data->frames == 0)
			return;
		std::vector<float> interleaved((size_t)data->frames * 2);
		auto *left = reinterpret_cast<const float *>(data->data[0]);
		auto *right = reinterpret_cast<const float *>(data->data[1]);
		for (uint32_t i = 0; i < data->frames; i++) {
			interleaved[(size_t)i * 2] = left[i];
			interleaved[(size_t)i * 2 + 1] = right[i];
		}
		/* OBS stamps each block with os_gettime_ns. The session clock is steady_clock. */
		if (!self->audio_origin_ns.load()) {
			uint64_t steady = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
						  std::chrono::steady_clock::now().time_since_epoch())
						  .count();
			uint64_t obs_now = os_gettime_ns();
			uint64_t age = obs_now > data->timestamp ? obs_now - data->timestamp : 0;
			self->audio_origin_ns.store(steady > age ? steady - age : 1);
		}
		uint64_t origin = self->audio_origin_ns.load();
		std::vector<airplay::AacPacket> packets;
		self->audio.push(interleaved.data(), data->frames, packets);
		for (const airplay::AacPacket &packet : packets) {
			auto rtp = airplay::build_rtp_audio(self->audio_seq++, packet.rtp_timestamp, self->audio_ssrc,
							    packet.data.data(), packet.data.size(), packet.marker);
			uint64_t captured = origin + (uint64_t)packet.rtp_timestamp * 1000000000ull / self->audio_rate;
			self->session.submit_audio(std::move(rtp), captured);
		}
	}

	void connect_selected()
	{
		if (session.waiting_for_pin()) {
			QString code = pin->text().trimmed();
			if (code.isEmpty()) {
				set_status(QString::fromUtf8("Type the code shown on the TV, then press Connect."));
				pin->setFocus();
				return;
			}
			pin->clear();
			session.submit_pin(code.toStdString());
			set_status(QString::fromUtf8("Pairing"));
			return;
		}
		const airplay::DiscoveredReceiver *device = selected();
		if (!device) {
			set_status(obs_module_text("StatusPick"));
			return;
		}
		if (device->reason != airplay::RejectReason::None) {
			set_status(QString::fromUtf8(airplay::reject_text(device->reason)));
			return;
		}
		if (device->address.empty()) {
			set_status(obs_module_text("StatusNoAddress"));
			return;
		}
		stop_pipeline();
		g_settings.device_id = device->device_id;
		g_settings.preview = preview->isChecked();
		if (audio_available)
			g_settings.send_audio = audio_box->isChecked();
		g_settings.bitrate_kbps = bitrate->value();
		obs_video_info info;
		src_width = 1920;
		src_height = 1080;
		src_fps = 60;
		fps_num = 60000;
		fps_den = 1001;
		if (obs_get_video_info(&info)) {
			src_width = info.base_width ? info.base_width : info.output_width;
			src_height = info.base_height ? info.base_height : info.output_height;
			src_fps = canvas_fps();
			if (info.fps_num && info.fps_den) {
				fps_num = info.fps_num;
				fps_den = info.fps_den;
			}
		}
		airplay::HandshakeInput input;
		input.receiver = *device;
		input.send_audio = audio_box->isChecked();
		input.allow_hevc = airplay::encoder_allows_hevc(g_settings.encoder_id);
		input.src_width = src_width;
		input.src_height = src_height;
		input.src_fps = src_fps;
		if (bitrate->value() > 0)
			input.bitrate_override_bps = (uint32_t)bitrate->value() * 1000u;
		pin->clear();
		QPointer<QDialog> alive(owner);
		uint64_t generation = ++connect_gen;
		session.start(
			input,
			[alive, generation](const std::string &text) {
				if (!alive)
					return;
				QString message = QString::fromStdString(text);
				QMetaObject::invokeMethod(
					alive.data(),
					[message, generation]() {
						if (!g_dialog || g_dialog->impl_->connect_gen != generation)
							return;
						g_dialog->impl_->set_status(message);
						if (g_dialog->impl_->session.waiting_for_pin()) {
							g_dialog->impl_->pin->setFocus();
							g_dialog->impl_->disconnect->setEnabled(true);
						}
					},
					Qt::QueuedConnection);
			},
			[alive, generation](const airplay::HandshakeResult &result) {
				if (!alive)
					return;
				QMetaObject::invokeMethod(
					alive.data(),
					[result, generation]() {
						if (!g_dialog || g_dialog->impl_->connect_gen != generation)
							return;
						g_dialog->impl_->max_width = result.max_width;
						g_dialog->impl_->max_height = result.max_height;
						g_dialog->impl_->max_fps = result.max_fps;
						QString error;
						if (!g_dialog->impl_->start_video(result.config, error)) {
							g_dialog->impl_->fail_pipeline(error);
							return;
						}
						g_dialog->impl_->begin_streaming();
						QString status = QString::fromStdString(result.status);
						if (result.config.codec != g_dialog->impl_->config.codec)
							status += QString(" · ") +
								  QString::fromUtf8(airplay::codec_name(g_dialog->impl_->config.codec));
						g_dialog->impl_->set_status(status);
					},
					Qt::QueuedConnection);
			});
		disconnect->setEnabled(true);
	}

	void refresh_stats()
	{
		if (!streaming) {
			stats->setText(obs_module_text("StatsIdle"));
			return;
		}
		uint64_t drops = video.dropped() + session.dropped();
		double latency_ms = (double)encode_ns.load() / 1e6;
		uint64_t got = video.frames();
		QString rate = QString::number(config.fps);
		if (fps_num && fps_den && (fps_num + fps_den / 2) / fps_den == config.fps && fps_num % fps_den != 0)
			rate = QString::number((double)fps_num / fps_den, 'f', 2);
		stats->setText(QString("%1 %2x%3 @ %4  ·  %5  ·  encode %6 ms  ·  dropped %7%8")
				       .arg(QString::fromUtf8(airplay::codec_name(config.codec)))
				       .arg(config.width)
				       .arg(config.height)
				       .arg(rate)
				       .arg(QString::fromStdString(video.encoder_name()))
				       .arg(latency_ms, 0, 'f', 1)
				       .arg((qulonglong)drops)
				       .arg(got == 0 ? QString::fromUtf8("  ·  no frames yet") : QString()));
	}
};

MonitorDialog::MonitorDialog(QWidget *parent) : QDialog(parent), impl_(new Impl)
{
	impl_->owner = this;
	setWindowTitle(obs_module_text("WindowTitle"));
	resize(560, 520);
	auto *layout = new QVBoxLayout(this);

	auto *receiver_row = new QHBoxLayout();
	receiver_row->addWidget(new QLabel(obs_module_text("Receivers")));
	receiver_row->addStretch();
	auto *refresh = new QPushButton(obs_module_text("Refresh"));
	receiver_row->addWidget(refresh);
	layout->addLayout(receiver_row);

	impl_->list = new QListWidget(this);
	layout->addWidget(impl_->list, 1);

	auto *source_box = new QGroupBox(obs_module_text("VideoSource"), this);
	auto *source_row = new QHBoxLayout(source_box);
	impl_->program = new QRadioButton(obs_module_text("Program"), source_box);
	impl_->preview = new QRadioButton(obs_module_text("Preview"), source_box);
	impl_->program->setChecked(!g_settings.preview);
	impl_->preview->setChecked(g_settings.preview);
	source_row->addWidget(impl_->program);
	source_row->addWidget(impl_->preview);
	layout->addWidget(source_box);

	impl_->audio_box = new QCheckBox(obs_module_text("SendAudio"), this);
	impl_->audio_box->setChecked(g_settings.send_audio);
	layout->addWidget(impl_->audio_box);
	std::string audio_reason;
	impl_->audio_available = airplay::AacEldEncoder::available(audio_reason);
	if (!impl_->audio_available) {
		impl_->audio_box->setChecked(false);
		impl_->audio_box->setEnabled(false);
		auto *missing = new QLabel(QString(obs_module_text("AudioUnavailable")).arg(audio_reason.c_str()), this);
		missing->setWordWrap(true);
		layout->addWidget(missing);
	}
	impl_->audio_note = new QLabel(obs_module_text("PreviewAudioNote"), this);
	impl_->audio_note->setWordWrap(true);
	layout->addWidget(impl_->audio_note);

	auto *encoder_row = new QHBoxLayout();
	encoder_row->addWidget(new QLabel(obs_module_text("VideoEncoder")));
	impl_->encoder_box = new QComboBox(this);
	impl_->encoder_box->addItem(obs_module_text("EncoderAuto"), QString());
	for (const airplay::VideoEncoderChoice &choice : airplay::video_encoder_choices())
		impl_->encoder_box->addItem(QString::fromStdString(choice.name), QString::fromStdString(choice.id));
	impl_->select_encoder();
	encoder_row->addWidget(impl_->encoder_box, 1);
	layout->addLayout(encoder_row);

	auto *bit_row = new QHBoxLayout();
	bit_row->addWidget(new QLabel(obs_module_text("Bitrate")));
	impl_->bitrate = new QSpinBox(this);
	impl_->bitrate->setRange(0, 40000);
	impl_->bitrate->setSingleStep(500);
	impl_->bitrate->setSpecialValueText(obs_module_text("BitrateAuto"));
	impl_->bitrate->setSuffix(obs_module_text("BitrateSuffix"));
	impl_->bitrate->setValue(g_settings.bitrate_kbps);
	bit_row->addWidget(impl_->bitrate);
	layout->addLayout(bit_row);

	auto *offset_row = new QHBoxLayout();
	offset_row->addWidget(new QLabel(obs_module_text("AudioOffset")));
	impl_->audio_offset = new QSpinBox(this);
	impl_->audio_offset->setRange(-100, 500);
	impl_->audio_offset->setSingleStep(10);
	impl_->audio_offset->setSuffix(obs_module_text("AudioOffsetSuffix"));
	impl_->audio_offset->setToolTip(obs_module_text("AudioOffsetTip"));
	impl_->audio_offset->setValue(g_settings.audio_offset_ms);
	impl_->session.set_audio_offset_ms(g_settings.audio_offset_ms);
	offset_row->addWidget(impl_->audio_offset);
	layout->addLayout(offset_row);

	auto *pin_row = new QHBoxLayout();
	pin_row->addWidget(new QLabel(obs_module_text("Pin")));
	impl_->pin = new QLineEdit(this);
	impl_->pin->setPlaceholderText(obs_module_text("PinHint"));
	impl_->pin->setMaxLength(8);
	impl_->pin->setClearButtonEnabled(true);
	QObject::connect(impl_->pin, &QLineEdit::returnPressed, this, [this]() { impl_->connect_selected(); });
	pin_row->addWidget(impl_->pin);
	layout->addLayout(pin_row);

	impl_->status = new QLabel(obs_module_text("StatusIdle"), this);
	impl_->status->setWordWrap(true);
	layout->addWidget(impl_->status);
	impl_->stats = new QLabel(obs_module_text("StatsIdle"), this);
	layout->addWidget(impl_->stats);

	auto *buttons = new QHBoxLayout();
	impl_->connect = new QPushButton(obs_module_text("Connect"), this);
	impl_->disconnect = new QPushButton(obs_module_text("Disconnect"), this);
	impl_->disconnect->setEnabled(false);
	buttons->addWidget(impl_->connect);
	buttons->addWidget(impl_->disconnect);
	layout->addLayout(buttons);

	QObject::connect(refresh, &QPushButton::clicked, this, [this]() {
		impl_->discovery.start([this](std::vector<airplay::DiscoveredReceiver> devices) {
			QMetaObject::invokeMethod(
				this,
				[this, devices = std::move(devices)]() mutable {
					impl_->devices = std::move(devices);
					impl_->rebuild_list();
				},
				Qt::QueuedConnection);
		});
		impl_->set_status(obs_module_text("StatusBrowsing"));
	});
	QObject::connect(impl_->connect, &QPushButton::clicked, this, [this]() { impl_->connect_selected(); });
	QObject::connect(impl_->disconnect, &QPushButton::clicked, this, [this]() {
		impl_->stop_pipeline();
		impl_->set_status(obs_module_text("StatusIdle"));
		impl_->refresh_stats();
	});
	QObject::connect(impl_->list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
		impl_->connect_selected();
	});
	QObject::connect(impl_->program, &QRadioButton::toggled, this, [this](bool on) {
		if (!on)
			return;
		g_settings.preview = false;
		impl_->refresh_audio_note();
		if (impl_->streaming && !impl_->retarget_video()) {
			QString message = impl_->status->text();
			impl_->fail_pipeline(message);
		}
	});
	QObject::connect(impl_->preview, &QRadioButton::toggled, this, [this](bool on) {
		if (!on)
			return;
		g_settings.preview = true;
		impl_->refresh_audio_note();
		if (impl_->streaming && !impl_->retarget_video()) {
			QString message = impl_->status->text();
			impl_->fail_pipeline(message);
		}
	});
	QObject::connect(impl_->audio_box, &QCheckBox::toggled, this, [this](bool on) {
		g_settings.send_audio = on;
		impl_->refresh_audio_note();
		if (impl_->streaming)
			impl_->set_status(obs_module_text("StatusAudioNext"));
	});
	QObject::connect(impl_->encoder_box, &QComboBox::currentIndexChanged, this, [this](int index) {
		g_settings.encoder_id = impl_->encoder_box->itemData(index).toString().toStdString();
		if (impl_->streaming)
			impl_->set_status(obs_module_text("StatusEncoderNext"));
	});
	QObject::connect(impl_->bitrate, &QSpinBox::valueChanged, this,
			 [this](int value) { g_settings.bitrate_kbps = value; });
	QObject::connect(impl_->audio_offset, &QSpinBox::valueChanged, this, [this](int value) {
		g_settings.audio_offset_ms = value;
		impl_->session.set_audio_offset_ms(value);
	});

	auto *timer = new QTimer(this);
	QObject::connect(timer, &QTimer::timeout, this, [this]() {
		impl_->refresh_stats();
		if (!impl_->streaming)
			return;
		obs_video_info info;
		if (!obs_get_video_info(&info))
			return;
		uint32_t width = info.base_width ? info.base_width : info.output_width;
		uint32_t height = info.base_height ? info.base_height : info.output_height;
		uint32_t fps = canvas_fps();
		if (width == impl_->src_width && height == impl_->src_height && fps == impl_->src_fps)
			return;
		impl_->src_width = width;
		impl_->src_height = height;
		impl_->src_fps = fps;
		on_video_reset();
	});
	timer->start(250);
	impl_->refresh_audio_note();
	refresh->click();
}

MonitorDialog::~MonitorDialog()
{
	impl_->video.stop();
	if (impl_->audio_on)
		obs_remove_raw_audio_callback(0, &Impl::on_audio, impl_);
	impl_->audio.stop();
	impl_->session.stop();
	impl_->discovery.stop();
	delete impl_;
}

void MonitorDialog::show_and_raise()
{
	show();
	raise();
	activateWindow();
}

void MonitorDialog::on_scenes()
{
	if (impl_->streaming && impl_->preview->isChecked())
		impl_->video.update_preview_scene();
}

void MonitorDialog::on_video_reset()
{
	if (!impl_->streaming)
		return;
	impl_->video.stop();
	uint32_t override_bps = impl_->bitrate->value() > 0 ? (uint32_t)impl_->bitrate->value() * 1000u : 0;
	bool hevc = impl_->config.codec == airplay::Codec::Hevc || impl_->config.fell_back_from_hevc;
	airplay::EncodeConfig wanted = airplay::choose_encode(
		impl_->src_width, impl_->src_height, impl_->src_fps, impl_->max_width, impl_->max_height, impl_->max_fps,
		hevc && !impl_->config.fell_back_from_hevc, override_bps);
	QString error;
	if (!impl_->start_video(wanted, error))
		impl_->fail_pipeline(error);
}

void MonitorDialog::collect(std::string &device_id, bool &preview, bool &send_audio, int &bitrate_kbps) const
{
	device_id = g_settings.device_id;
	preview = impl_->preview->isChecked();
	send_audio = impl_->audio_available ? impl_->audio_box->isChecked() : g_settings.send_audio;
	bitrate_kbps = impl_->bitrate->value();
}

void MonitorDialog::select_encoder()
{
	impl_->select_encoder();
}

void MonitorDialog::set_audio_offset(int ms)
{
	impl_->audio_offset->setValue(ms);
	impl_->session.set_audio_offset_ms(ms);
}

void MonitorDialog::restore(const std::string &device_id, bool preview, bool send_audio, int bitrate_kbps)
{
	g_settings.device_id = device_id;
	g_settings.preview = preview;
	g_settings.send_audio = send_audio;
	g_settings.bitrate_kbps = bitrate_kbps;
	impl_->program->setChecked(!preview);
	impl_->preview->setChecked(preview);
	impl_->audio_box->setChecked(send_audio && impl_->audio_available);
	impl_->bitrate->setValue(bitrate_kbps);
	impl_->rebuild_list();
	impl_->refresh_audio_note();
}

void MonitorDialog::closeEvent(QCloseEvent *event)
{
	event->ignore();
	hide();
}

void airplay_monitor_show()
{
	if (!g_dialog) {
		auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
		g_dialog = new MonitorDialog(parent);
		g_dialog->restore(g_settings.device_id, g_settings.preview, g_settings.send_audio, g_settings.bitrate_kbps);
	}
	g_dialog->show_and_raise();
}

void airplay_monitor_shutdown()
{
	if (!g_dialog)
		return;
	MonitorDialog *dialog = g_dialog;
	g_dialog = nullptr;
	delete dialog;
}

void airplay_monitor_on_scenes()
{
	if (g_dialog)
		g_dialog->on_scenes();
}

void airplay_monitor_on_video_reset()
{
	if (g_dialog)
		g_dialog->on_video_reset();
}

void airplay_monitor_collect(std::string &device_id, bool &preview, bool &send_audio, int &bitrate_kbps)
{
	if (g_dialog)
		g_dialog->collect(device_id, preview, send_audio, bitrate_kbps);
	else {
		device_id = g_settings.device_id;
		preview = g_settings.preview;
		send_audio = g_settings.send_audio;
		bitrate_kbps = g_settings.bitrate_kbps;
	}
}

void airplay_monitor_restore(const std::string &device_id, bool preview, bool send_audio, int bitrate_kbps)
{
	g_settings.device_id = device_id;
	g_settings.preview = preview;
	g_settings.send_audio = send_audio;
	g_settings.bitrate_kbps = bitrate_kbps;
	if (g_dialog)
		g_dialog->restore(device_id, preview, send_audio, bitrate_kbps);
}

std::string airplay_monitor_encoder()
{
	return g_settings.encoder_id;
}

void airplay_monitor_set_encoder(const std::string &id)
{
	g_settings.encoder_id = id;
	if (g_dialog)
		g_dialog->select_encoder();
}

int airplay_monitor_audio_offset()
{
	return g_settings.audio_offset_ms;
}

void airplay_monitor_set_audio_offset(int ms)
{
	g_settings.audio_offset_ms = ms;
	if (g_dialog)
		g_dialog->set_audio_offset(ms);
}
