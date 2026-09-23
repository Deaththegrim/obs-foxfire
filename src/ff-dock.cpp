/*
Foxfire
Copyright (C) 2026 KitsuneStudio

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* The control panel, inside OBS.
 *
 * The only file in this project that knows Qt exists, and the only one compiled as C++. It reads
 * every Foxfire source through the per-source procedures ff-props.c registers (see
 * ff-dock-proto.h) and never touches a struct ff_instance: nothing hands one out, and the point
 * of the tap is that nothing needs to.
 *
 * Thread: everything here runs on the Qt/UI thread. The threading contract at the top of
 * ff-props.c already names that thread as one of the two that reach an instance -- "Both the Qt
 * properties dialog and obs-websocket land here" -- so a dock is the same category of actor as
 * the properties panel, not a new one. The meter read takes no lock at all: it goes through the
 * handoff seqlock, which several readers may use at once and which never makes the audio thread
 * wait.
 */

#include "ff-dock.h"
#include "ff-dock-proto.h"
#include "ff-frame.h"
#include <plugin-support.h>

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

/* ~30 Hz. Fast enough that a meter reads as continuous, slow enough to cost nothing: each tick is
   one obs_enum_sources pass and one seqlock copy per source. */
constexpr int METER_MS = 33;
/* The source list changes at human speed. Rescanning it 30 times a second would be the only
   expensive thing this dock does. */
constexpr int RESCAN_MS = 1000;

const char *const kDockId = "foxfire_dock";

bool is_foxfire(obs_source_t *s)
{
	const char *id = obs_source_get_id(s);
	if (!id)
		return false;
	/* The three ids ff-source.c, ff-filter.c and ff-transition.c register. foxfire_alert belongs
	   to the other plugin module, which this dock is not part of. */
	return strcmp(id, "foxfire_visualizer") == 0 || strcmp(id, "foxfire_effects") == 0 ||
	       strcmp(id, "foxfire_transition") == 0;
}

/* One source's bars. Painted rather than assembled from child widgets: 64 bands would otherwise be
   64 QWidgets repainting 30 times a second. */
class MeterWidget : public QWidget {
public:
	explicit MeterWidget(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(44);
		memset(&frame_, 0, sizeof frame_);
	}

	void setFrame(const struct ff_frame &f)
	{
		frame_ = f;
		update();
	}

	/* Drawn even with no data: an empty meter is a meter reading silence, which is information.
	   A widget that paints nothing until audio arrives is indistinguishable from a broken one. */
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const int w = width(), h = height();
		p.fillRect(0, 0, w, h, QColor(24, 24, 28));
		if (w <= 0 || h <= 0)
			return;
		const float bw = float(w) / float(FF_BANDS);
		for (int i = 0; i < FF_BANDS; i++) {
			float v = frame_.bands[i];
			if (v < 0.0f)
				v = 0.0f;
			if (v > 1.0f)
				v = 1.0f;
			const int bh = int(v * float(h - 2));
			const int x = int(float(i) * bw);
			const int cw = int(bw) > 1 ? int(bw) - 1 : 1;
			p.fillRect(x, h - bh, cw, bh, QColor(255, 106, 77));
			/* the peak hold, so a transient that is gone by the next repaint still shows */
			float pk = frame_.peaks[i];
			if (pk > 0.0f) {
				if (pk > 1.0f)
					pk = 1.0f;
				const int py = h - int(pk * float(h - 2));
				p.fillRect(x, py, cw, 2, QColor(242, 233, 223));
			}
		}
	}

private:
	struct ff_frame frame_;
};

/* One row: a source's name, what it is hearing, and its meters. */
class SourceRow : public QWidget {
public:
	explicit SourceRow(const QString &name, QWidget *parent = nullptr) : QWidget(parent), name_(name)
	{
		auto *lay = new QVBoxLayout(this);
		lay->setContentsMargins(6, 4, 6, 6);
		lay->setSpacing(2);
		title_ = new QLabel(name, this);
		title_->setStyleSheet("font-weight: bold;");
		status_ = new QLabel(QString(), this);
		status_->setWordWrap(true);
		meter_ = new MeterWidget(this);
		lay->addWidget(title_);
		lay->addWidget(status_);
		lay->addWidget(meter_);
	}

	const QString &sourceName() const { return name_; }

	/* A deterministic frame, for the grab diagnostic only. A meter reading silence is LEGITIMATELY
	   flat -- and a meter that paints nothing at all is flat in exactly the same way, so a picture
	   of an idle dock cannot tell the two apart. Headless OBS has no audio, so the only way to gate
	   the paint path is to hand it something known and require the picture to change. Measured: a
	   dock whose MeterWidget::paintEvent returns immediately passed every is-it-blank check there
	   is, because the row's own labels carry the variance. */
	void seedTestFrame()
	{
		struct ff_frame f;
		memset(&f, 0, sizeof f);
		for (int i = 0; i < FF_BANDS; i++) {
			f.bands[i] = float(i) / float(FF_BANDS - 1);
			f.peaks[i] = f.bands[i];
		}
		f.level = 1.0f;
		meter_->setFrame(f);
	}

	void tickMeter(obs_source_t *src)
	{
		struct ff_frame f;
		memset(&f, 0, sizeof f);
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_ptr(&cd, FF_CD_OUT_FRAME, &f);
		const bool called = proc_handler_call(obs_source_get_proc_handler(src), FF_PROC_METER_READ, &cd);
		const bool valid = called && calldata_bool(&cd, FF_CD_VALID);
		calldata_free(&cd);
		/* An invalid read is not an error and must not blank the meter: nothing has been
		   published yet, or this poll lost the seqlock race. Either way the last frame is
		   still the truest thing we have. */
		if (valid)
			meter_->setFrame(f);
	}

	void tickStatus(obs_source_t *src)
	{
		struct ff_dock_status st;
		memset(&st, 0, sizeof st);
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_ptr(&cd, FF_CD_OUT_STATUS, &st);
		const bool ok = proc_handler_call(obs_source_get_proc_handler(src), FF_PROC_DOCK_STATUS, &cd);
		calldata_free(&cd);
		if (!ok)
			return;

		QString line;
		if (st.audio_mode == FF_AUDIO_MASTER)
			line = QStringLiteral("Master audio");
		else if (st.audio_source[0])
			line = QStringLiteral("Audio: %1").arg(QString::fromUtf8(st.audio_source));
		else
			line = QStringLiteral("Audio: (no source chosen)");

		if (st.preset_id[0])
			line += QStringLiteral("  ·  %1 / %2")
					.arg(QString::fromUtf8(st.pack_id), QString::fromUtf8(st.preset_id));

		/* A fault wins the line. The panel and the dock say the same sentence about the same
		   problem because both get it from ff_audio_describe -- see ff-audio.c. */
		if (!st.audio_ok && st.audio_msg[0]) {
			line = QString::fromUtf8(st.audio_msg);
			status_->setStyleSheet("color: #ff6a4d;");
		} else if (st.status[0]) {
			line = QString::fromUtf8(st.status);
			status_->setStyleSheet("color: #ff6a4d;");
		} else {
			status_->setStyleSheet(QString());
		}
		status_->setText(line);
	}

private:
	QString name_;
	QLabel *title_;
	QLabel *status_;
	MeterWidget *meter_;
};

class FoxfireDock : public QWidget {
public:
	explicit FoxfireDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName("foxfire_dock_root");
		layout_ = new QVBoxLayout(this);
		layout_->setContentsMargins(0, 0, 0, 0);
		layout_->setSpacing(0);
		empty_ = new QLabel(obs_module_text("Foxfire.Dock.NoSources"), this);
		empty_->setWordWrap(true);
		empty_->setContentsMargins(8, 8, 8, 8);
		layout_->addWidget(empty_);
		layout_->addStretch(1);

		rescan_ = new QTimer(this);
		QObject::connect(rescan_, &QTimer::timeout, this, [this]() { rescan(); });
		rescan_->start(RESCAN_MS);

		meters_ = new QTimer(this);
		QObject::connect(meters_, &QTimer::timeout, this, [this]() { tick(); });
		/* started by rescan() only when there is something to meter -- a 30 Hz timer painting
		   nothing runs for the whole OBS session otherwise */

		rescan();
	}

	/* see SourceRow::seedTestFrame -- the grab diagnostic's half of gating the paint path */
	void seedMeters()
	{
		for (auto *r : rows_)
			r->seedTestFrame();
	}

private:
	struct Found {
		std::vector<QString> names;
	};

	static bool collect(void *param, obs_source_t *s)
	{
		if (is_foxfire(s)) {
			auto *f = static_cast<Found *>(param);
			const char *n = obs_source_get_name(s);
			f->names.emplace_back(QString::fromUtf8(n ? n : ""));
		}
		return true; /* keep enumerating */
	}

	void rescan()
	{
		Found found;
		obs_enum_sources(collect, &found);

		/* Rebuild only when the set actually changed: replacing the rows every second would
		   throw away the peak-hold state the meters are carrying and flicker the layout. */
		bool same = found.names.size() == rows_.size();
		if (same)
			for (size_t i = 0; i < rows_.size(); i++)
				if (rows_[i]->sourceName() != found.names[i]) {
					same = false;
					break;
				}
		if (!same) {
			for (auto *r : rows_) {
				layout_->removeWidget(r);
				r->deleteLater();
			}
			rows_.clear();
			for (size_t i = 0; i < found.names.size(); i++) {
				auto *row = new SourceRow(found.names[i], this);
				layout_->insertWidget(int(i), row);
				rows_.push_back(row);
			}
		}
		/* Logged on CHANGE only, so it is not 30 lines a second, and logged at all because it
		   is the one thing a pixel gate cannot read off a grab: a dock showing "no sources"
		   renders perfectly well and passes any non-blankness check. tools/dock-proof.py asserts
		   on this line. */
		if (!same)
			obs_log(LOG_INFO, "dock: %d source(s)", int(rows_.size()));
		empty_->setVisible(rows_.empty());
		if (rows_.empty())
			meters_->stop();
		else if (!meters_->isActive())
			meters_->start(METER_MS);

		for (auto *r : rows_)
			withSource(r, [r](obs_source_t *s) { r->tickStatus(s); });
	}

	void tick()
	{
		for (auto *r : rows_)
			withSource(r, [r](obs_source_t *s) { r->tickMeter(s); });
	}

	/* A strong reference for exactly the length of one call and never across a tick. The source
	   can be destroyed between the rescan that listed it and the tick that reads it; looking it
	   up by name each time turns that into "nothing happens this tick" instead of a stale
	   pointer. */
	template<typename F> static void withSource(SourceRow *row, F fn)
	{
		obs_source_t *s = obs_get_source_by_name(row->sourceName().toUtf8().constData());
		if (!s)
			return;
		fn(s);
		obs_source_release(s);
	}

	QVBoxLayout *layout_;
	QLabel *empty_;
	QTimer *rescan_;
	QTimer *meters_;
	std::vector<SourceRow *> rows_;
};

FoxfireDock *g_dock = nullptr;

/* The dock draws ITSELF to a PNG. Nothing else can check it: a Qt widget OBS owns is invisible to
   GetSourceScreenshot and to every pixel gate in this repo, and there is no window manager under
   Xvfb to screenshot around. Measured in a spike before any of this was written -- a rendered
   dock grabs at pixel std 41.22, an empty widget of the same size at 0.00 -- so "did the dock
   actually lay out and paint" is answerable headless, which is what lets tools/dock-proof.py gate
   it rather than merely photograph it. */
void grab_to(const char *path)
{
	if (!g_dock)
		return;
	g_dock->seedMeters();
	const QImage img = g_dock->grab().toImage();
	const bool saved = !img.isNull() && img.save(QString::fromUtf8(path), "PNG");
	obs_log(LOG_INFO, "dock-grab: %dx%d saved=%s -> %s", img.width(), img.height(), saved ? "TRUE" : "FALSE", path);
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || g_dock)
		return;
	g_dock = new FoxfireDock();
	const bool ok = obs_frontend_add_dock_by_id(kDockId, obs_module_text("Foxfire.Dock.Title"), g_dock);
	obs_log(LOG_INFO, "dock: registered=%s", ok ? "TRUE" : "FALSE");

	/* Diagnostic hook, same shape as FOXFIRE_INSTALL_ZIP and FOXFIRE_PROC_PROOF in
	   plugin-main.c. The delay is not decoration: a widget grabbed before the event loop has
	   laid it out is blank, and a blank grab is exactly what the gate refuses. */
	if (const char *out = getenv("FOXFIRE_DOCK_GRAB"))
		QTimer::singleShot(3000, g_dock, [out]() { grab_to(out); });
}

} /* namespace */

void ff_dock_register(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void ff_dock_unregister(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_dock) {
		/* OBS owns the widget once it has been docked; removing the dock is what disposes of
		   it, and deleting it here as well would be a double free. */
		obs_frontend_remove_dock(kDockId);
		g_dock = nullptr;
	}
}
