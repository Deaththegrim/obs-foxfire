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
/* for S_PACK / S_PRESET. Included rather than retyping the two strings here: the settings keys are
   the contract between this dock and ff_instance_update, and a dock writing "preset" while the
   instance reads something else is a preset switch that silently does nothing. */
#include "ff-props.h"
#include <plugin-support.h>

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
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

/* Which combos a row is waiting to write. A bitmask, so a pack change and a preset change landing
   in the same tick are both honoured -- see SourceRow::chosen. */
constexpr int FF_PENDING_PACK = 1;
constexpr int FF_PENDING_PRESET = 2;

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
	/* Takes ownership of `weak`. Held for the row's life, resolved to a strong reference for the
	   length of one call and no longer -- see FoxfireDock::withSource. The row used to look its
	   source up by NAME every tick: a hash lookup per source per tick, and, when two sources share
	   a name, silently the wrong one. obs.h documents obs_source_get_weak_source for exactly this. */
	SourceRow(const QString &name, obs_weak_source_t *weak, QWidget *parent = nullptr)
		: QWidget(parent),
		  name_(name),
		  weak_(weak)
	{
		auto *lay = new QVBoxLayout(this);
		lay->setContentsMargins(6, 4, 6, 6);
		lay->setSpacing(2);
		title_ = new QLabel(name, this);
		title_->setStyleSheet("font-weight: bold;");
		status_ = new QLabel(QString(), this);
		status_->setWordWrap(true);

		auto *picker = new QHBoxLayout();
		picker->setSpacing(4);
		pack_ = new QComboBox(this);
		preset_ = new QComboBox(this);
		/* A dock is narrow, and the default policy shrank the pack box until it read "Fox" --
		   measured at 198 px wide, which is a perfectly ordinary docked width. A minimum content
		   length keeps the names readable, and Expanding lets the two share whatever width the
		   streamer gives the panel rather than both staying stubbornly small. */
		for (QComboBox *c : {pack_, preset_}) {
			c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
			c->setMinimumContentsLength(9);
			c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		}
		picker->addWidget(pack_, 1);
		picker->addWidget(preset_, 2);

		meter_ = new MeterWidget(this);
		lay->addWidget(title_);
		lay->addWidget(status_);
		lay->addLayout(picker);
		lay->addWidget(meter_);

		QObject::connect(pack_, &QComboBox::currentTextChanged, this, [this]() { chosen(true); });
		QObject::connect(preset_, &QComboBox::currentTextChanged, this, [this]() { chosen(false); });
	}

	~SourceRow() override
	{
		if (weak_)
			obs_weak_source_release(weak_);
	}

	const QString &sourceName() const { return name_; }
	obs_weak_source_t *weak() const { return weak_; }

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

		/* A fault is APPENDED, not substituted. Replacing the line dropped the pack and preset
		   exactly when they matter most: "Audio source 'Mic' not found" alone does not say which
		   preset is loaded, and the two together are what tell a missing pack apart from a
		   missing audio source. The panel and the dock say the same sentence about the same
		   problem because both get it from ff_audio_describe -- see ff-audio.c. */
		const char *fault = (!st.audio_ok && st.audio_msg[0]) ? st.audio_msg : (st.status[0] ? st.status : nullptr);
		if (fault) {
			line += QStringLiteral("  ·  %1").arg(QString::fromUtf8(fault));
			status_->setStyleSheet("color: #ff6a4d;");
		} else {
			status_->setStyleSheet(QString());
		}
		status_->setText(line);
	}

	/* The pack and preset lists, taken from the source's OWN properties rather than scanned here.
	   ff-props.c already decides which packs have a preset of this instance's kind, keeps a
	   selected-but-mismatched pack in the list so a saved value never silently jumps, and repairs
	   a preset that is not in the chosen pack. A second enumerator in the dock would be a second
	   set of those decisions, free to disagree with the panel about the same pack.

	   Not called per tick: obs_source_properties runs a full pack rescan (readdir, JSON parse and
	   an Ed25519 verify per pack), which at 1 Hz per source would be the most expensive thing this
	   dock does. Called when the row is built and when the pack actually changes. */
	/* Returns whether `want_preset` was in the rebuilt list. The pack switch needs that answer:
	   see FoxfireDock::apply. */
	bool refreshLists(obs_source_t *src, const char *want_pack, const char *want_preset)
	{
		obs_properties_t *props = obs_source_properties(src);
		if (!props)
			return false;
		const QSignalBlocker b1(pack_), b2(preset_);
		fill(pack_, obs_properties_get(props, S_PACK), want_pack);
		const bool have = fill(preset_, obs_properties_get(props, S_PRESET), want_preset);
		obs_properties_destroy(props);
		return have;
	}

	/* The first preset the list offers, or empty if it offers none -- the fallback when a pack
	   change leaves the old preset with nowhere to land. */
	QString firstPreset() const
	{
		return preset_->count() > 0 ? preset_->itemData(0).toString() : QString();
	}

	void syncSelection(const char *pack, const char *preset)
	{
		const QSignalBlocker b1(pack_), b2(preset_);
		select(pack_, pack);
		select(preset_, preset);
	}

	/* selects by preset id, as a click would; false if the list does not offer it */
	bool choosePreset(const char *preset_id)
	{
		const int i = preset_->findData(QString::fromUtf8(preset_id));
		if (i < 0)
			return false;
		preset_->setCurrentIndex(i); /* fires chosen(false), setting pending_ */
		return true;
	}

	/* the same, for the pack combo -- fires chosen(true) */
	bool choosePack(const char *pack_id)
	{
		const int i = pack_->findData(QString::fromUtf8(pack_id));
		if (i < 0)
			return false;
		pack_->setCurrentIndex(i);
		return true;
	}

	QString chosenPack() const { return pack_->currentData().toString(); }
	QString chosenPreset() const { return preset_->currentData().toString(); }
	/* which combos the user just touched, consumed by the dock's apply step */
	int takePending()
	{
		const int p = pending_;
		pending_ = 0;
		return p;
	}
	/* A selection the user has made and the dock has not written yet. The 1 Hz rescan must not
	   overwrite the combo while one is outstanding: its syncSelection would put the OLD value back
	   from the source's settings, and the apply a few ms later would then read that old value back
	   out of the combo and write the setting the source already had -- the click would silently do
	   nothing, roughly once in every sixty, whenever a rescan landed inside the 33 ms window. */
	bool hasPending() const { return pending_ != 0; }

	/* Where this row's meter sits in `root`'s coordinates, for the grab gate -- which has to count
	   bar pixels somewhere it knows is a meter. The bars are #ff6a4d, and so is the fault status
	   label two widgets up, so counting that colour over the whole dock counts red TEXT: a meter
	   that painted nothing still passed whenever a row was showing a fault, which is exactly when
	   the gate is worth having. */
	QRect meterRectIn(const QWidget *root) const
	{
		const QPoint tl = meter_->mapTo(root, QPoint(0, 0));
		return QRect(tl, meter_->size());
	}

private:
	static bool fill(QComboBox *box, obs_property_t *p, const char *want)
	{
		box->clear();
		if (!p)
			return false;
		const size_t n = obs_property_list_item_count(p);
		for (size_t i = 0; i < n; i++) {
			const char *nm = obs_property_list_item_name(p, i);
			const char *val = obs_property_list_item_string(p, i);
			box->addItem(QString::fromUtf8(nm ? nm : ""), QString::fromUtf8(val ? val : ""));
		}
		return select(box, want);
	}

	/* false when the list does not offer `want` -- which leaves the combo on whatever item 0 is,
	   showing a selection the source does not have. Returned rather than silently ignored because
	   the pack switch below has to know: it is the difference between "the new pack kept the
	   preset" and "the preset is gone and something has to choose another one". */
	static bool select(QComboBox *box, const char *want)
	{
		if (!want || !*want)
			return false;
		const int i = box->findData(QString::fromUtf8(want));
		if (i < 0)
			return false;
		if (i != box->currentIndex())
			box->setCurrentIndex(i);
		return true;
	}

	/* A BITMASK: bit 0 = the pack changed (the preset list must be rebuilt for it), bit 1 = the
	   preset did. Both can be outstanding at once -- a pack change rebuilds the preset list, and
	   the user can pick from it inside the same 33 ms tick -- and an assignment would have kept
	   only whichever arrived last, applying a pack with the old pack's preset.

	   Recorded rather than acted on: applying a setting reaches into libobs, and doing that from
	   inside a Qt signal handler means it happens in the middle of the combo's own update. */
	void chosen(bool isPack) { pending_ |= isPack ? FF_PENDING_PACK : FF_PENDING_PRESET; }

	QString name_;
	obs_weak_source_t *weak_;
	QLabel *title_;
	QLabel *status_;
	QComboBox *pack_;
	QComboBox *preset_;
	MeterWidget *meter_;
	int pending_ = 0;
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

	/* see the FOXFIRE_DOCK_PICK / FOXFIRE_DOCK_PICK_PACK hooks */
	void pick(const char *want, bool isPack)
	{
		const char *what = isPack ? "pack" : "preset";
		if (rows_.empty()) {
			obs_log(LOG_INFO, "dock-pick: no rows");
			return;
		}
		SourceRow *row = rows_.front();

		/* Logged BEFORE anything is written, and read off the SOURCE rather than the combo.
		   The gate needs to know what the source started on: without it, "it ended on what we
		   asked for" also passes when it was already there and nothing happened -- the check
		   passing for the exact reason it exists to rule out. */
		withSource(row, [](obs_source_t *s) {
			obs_data_t *st = obs_source_get_settings(s);
			obs_log(LOG_INFO, "dock-pick: source was on '%s' / '%s'",
				obs_data_get_string(st, S_PACK), obs_data_get_string(st, S_PRESET));
			obs_data_release(st);
		});

		if (!(isPack ? row->choosePack(want) : row->choosePreset(want))) {
			obs_log(LOG_INFO, "dock-pick: '%s' is not in the %s list", want, what);
			return;
		}
		obs_log(LOG_INFO, "dock-pick: asked for %s '%s'", what, want);

		/* Deliberately NOT applied here. A click sets the pending flag and returns; the write
		   happens on a later tick, and a rescan can land in between. Consuming the flag here
		   would skip both, so the hook would take a path no click takes -- and the race between
		   the rescan and the pending apply is exactly the one worth gating. This wait spans
		   several ticks and at least two full rescans. */
		QTimer::singleShot(2500, this, [this, row]() {
			if (std::find(rows_.begin(), rows_.end(), row) == rows_.end()) {
				obs_log(LOG_INFO, "dock-pick: the row went away before the write landed");
				return;
			}
			withSource(row, [](obs_source_t *s) {
				obs_data_t *st = obs_source_get_settings(s);
				obs_log(LOG_INFO, "dock-pick: source now on '%s' / '%s'",
					obs_data_get_string(st, S_PACK), obs_data_get_string(st, S_PRESET));
				obs_data_release(st);
			});
		});
	}

	/* see grab_to -- one line per meter, in the grab's own coordinates */
	void logMeterRects()
	{
		for (size_t i = 0; i < rows_.size(); i++) {
			const QRect r = rows_[i]->meterRectIn(this);
			obs_log(LOG_INFO, "dock-grab: meter %d at %d,%d %dx%d", int(i), r.x(), r.y(),
				r.width(), r.height());
		}
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
		std::vector<obs_weak_source_t *> weaks; /* owned until handed to a row or released */
		obs_source_t *current_transition = nullptr; /* borrowed for the length of the scan */
	};

	static bool collect(void *param, obs_source_t *s)
	{
		auto *f = static_cast<Found *>(param);
		if (!is_foxfire(s))
			return true;
		/* Transitions: only the one that is actually in use. Every transition a collection
		   defines is a live source, so enumerating all of them put one row per CONFIGURED
		   transition in the panel -- measured at 31 rows for a collection with two scenes,
		   twenty-nine of them transitions nobody was using, each metering at 30 Hz. A streamer
		   cuts with one at a time, and that one is worth a row. */
		if (strcmp(obs_source_get_id(s), "foxfire_transition") == 0 && s != f->current_transition)
			return true;
		const char *n = obs_source_get_name(s);
		f->names.emplace_back(QString::fromUtf8(n ? n : ""));
		/* the enumerator does not keep the source alive past this call; obs.h names
		   obs_source_get_weak_source as the way to retain one, which is what a row needs. */
		f->weaks.push_back(obs_source_get_weak_source(s));
		return true; /* keep enumerating */
	}

	void rescan()
	{
		Found found;
		/* ALL sources, not obs_enum_sources. obs.h: obs_enum_sources "Enumerates all INPUT
		   sources", obs_enum_all_sources "Enumerates all sources (regardless of type)" -- so the
		   narrow one never yields a filter or a transition, and two of the three ids is_foxfire
		   matches were unreachable. The dock listed only visualizers while telling anyone with a
		   Foxfire filter or transition that there were no Foxfire sources at all. */
		/* A strong ref for the scan only; collect compares against it and never keeps it. */
		found.current_transition = obs_frontend_get_current_transition();
		obs_enum_all_sources(collect, &found);
		if (found.current_transition)
			obs_source_release(found.current_transition);
		found.current_transition = nullptr;

		/* Rebuild only when the set actually changed: replacing the rows every second would
		   throw away the peak-hold state the meters are carrying and flicker the layout. */
		/* Compared by WEAK REF, not by name: two sources may share a name, and a rename is not a
		   different source. Comparing names rebuilt every row on a rename and never noticed a
		   swap between two identically named ones. */
		bool same = found.weaks.size() == rows_.size();
		if (same)
			for (size_t i = 0; i < rows_.size(); i++)
				if (rows_[i]->weak() != found.weaks[i]) {
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
				/* the row takes ownership of the weak ref */
				auto *row = new SourceRow(found.names[i], found.weaks[i], this);
				layout_->insertWidget(int(i), row);
				rows_.push_back(row);
				withSource(row, [row](obs_source_t *src) {
					obs_data_t *st = obs_source_get_settings(src);
					row->refreshLists(src, obs_data_get_string(st, S_PACK),
							  obs_data_get_string(st, S_PRESET));
					obs_data_release(st);
				});
			}
			found.weaks.clear(); /* every one handed to a row */
		} else {
			/* nothing was rebuilt, so nothing took them */
			for (auto *w : found.weaks)
				obs_weak_source_release(w);
			found.weaks.clear();
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
			withSource(r, [r](obs_source_t *s) {
				r->tickStatus(s);
				/* NOT while the row has a selection waiting to be written. The combos are
				   the only record of what the user just picked -- putting the source's
				   current values back would revert it, and the apply on the next tick
				   reads the combo, so the click would write the setting the source
				   already had and appear to do nothing. See SourceRow::hasPending. */
				if (r->hasPending())
					return;
				obs_data_t *st = obs_source_get_settings(s);
				/* keep the combos showing what the instance actually has: the properties
				   panel, obs-websocket or a scene-collection load can all change it */
				r->syncSelection(obs_data_get_string(st, S_PACK), obs_data_get_string(st, S_PRESET));
				obs_data_release(st);
			});
	}

	void tick()
	{
		for (auto *r : rows_) {
			/* Applied here rather than in the combo's own signal handler: writing a setting
			   calls into libobs, and doing that from inside the widget's update is how a
			   re-entrant repaint turns into a crash. The next tick is 33 ms away. */
			const int pending = r->takePending();
			if (pending)
				withSource(r, [r, pending](obs_source_t *s) { apply(r, s, pending); });
			withSource(r, [r](obs_source_t *s) { r->tickMeter(s); });
		}
	}

	/* The preset switch, through the SAME path the properties panel drives: the two settings keys
	   and obs_source_update. Not a private back door -- going this way keeps the licence gate, the
	   erase-layer-keys-on-change and every refusal ff_instance_update makes. */
	static void apply(SourceRow *row, obs_source_t *src, int pending)
	{
		const QByteArray pk = row->chosenPack().toUtf8();
		if (pk.isEmpty())
			return;

		if (pending & FF_PENDING_PACK) {
			/* Send the pack ALONE first. The preset combo still holds the old pack's
			   selection, which very likely is not in the new pack, and asking for a preset
			   that does not exist is what ff_instance_update answers by loading zero layers
			   and setting "preset missing" -- a blank source. */
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, S_PACK, pk.constData());
			obs_source_update(src, st);
			obs_data_release(st);

			/* Rebuild the lists for the new pack, then CHECK where that left the preset
			   rather than assuming.

			   What normally happens, measured rather than reasoned: obs_source_properties
			   below runs ff-props.c's on_pack_changed, which writes a preset valid for the
			   new pack into the live settings, and libobs defers a video source's update to
			   the graphics tick, so the deferred ff_instance_update reads the repaired value
			   and never sees the stale one. `kept` is therefore true on the ordinary path and
			   the block below does not run.

			   It is kept anyway because that is three libobs behaviours this file does not
			   control, in a specific order. Both halves were mutation-tested: with the rebuild
			   no longer feeding a repaired preset, this block alone still lands the source on
			   a preset the new pack has; with BOTH gone, the source ends on basics' "bars"
			   inside the "ring" pack -- a preset that does not exist, which ff_instance_update
			   answers by loading zero layers. That is a blank canvas, and tools/dock-proof.py
			   now fails on it. */
			obs_data_t *now = obs_source_get_settings(src);
			const bool kept = row->refreshLists(src, obs_data_get_string(now, S_PACK),
							    obs_data_get_string(now, S_PRESET));
			obs_data_release(now);
			if (!kept) {
				const QByteArray first = row->firstPreset().toUtf8();
				obs_log(LOG_INFO, "dock: pack '%s' does not keep the preset; falling back to '%s'",
					pk.constData(), first.isEmpty() ? "(none)" : first.constData());
				if (!first.isEmpty()) {
					obs_data_t *fix = obs_data_create();
					obs_data_set_string(fix, S_PRESET, first.constData());
					obs_source_update(src, fix);
					obs_data_release(fix);
					row->refreshLists(src, pk.constData(), first.constData());
				}
			}
			return; /* the preset is now whatever the new pack settled on, already written */
		}

		/* A preset-only change. The pack is sent as well so the two always agree, but only when
		   it MATCHES what the source already has: writing the combo's pack unconditionally could
		   migrate a source whose pack was changed underneath us (properties panel, websocket, a
		   collection load) between the rescan and this tick -- and a pack change is not free, it
		   runs erase_layer_keys and drops the instance's per-layer settings. */
		const QByteArray pr = row->chosenPreset().toUtf8();
		if (pr.isEmpty())
			return;
		obs_data_t *cur = obs_source_get_settings(src);
		const bool same_pack = pk == obs_data_get_string(cur, S_PACK);
		obs_data_release(cur);

		obs_data_t *st = obs_data_create();
		if (same_pack)
			obs_data_set_string(st, S_PACK, pk.constData());
		obs_data_set_string(st, S_PRESET, pr.constData());
		obs_source_update(src, st);
		obs_data_release(st);
	}

	/* A strong reference for exactly the length of one call and never across a tick. The source
	   can be destroyed between the rescan that listed it and the tick that reads it; looking it
	   up by name each time turns that into "nothing happens this tick" instead of a stale
	   pointer. */
	template<typename F> static void withSource(SourceRow *row, F fn)
	{
		if (!row->weak())
			return;
		obs_source_t *s = obs_weak_source_get_source(row->weak());
		if (!s)
			return; /* destroyed since the rescan that listed it; the next one drops the row */
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
	/* Where the meters are, so the gate can count bar pixels somewhere it KNOWS is a meter. The
	   bars are #ff6a4d and so is the fault status label, so counting that colour across the whole
	   grab counts red text: a meter painting nothing passed the moment any row showed a fault. */
	g_dock->logMeterRects();
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || g_dock)
		return;
	g_dock = new FoxfireDock();
	const bool ok = obs_frontend_add_dock_by_id(kDockId, obs_module_text("Foxfire.Dock.Title"), g_dock);
	obs_log(LOG_INFO, "dock: registered=%s", ok ? "TRUE" : "FALSE");
	if (!ok) {
		/* Nothing else owns it. The widget is parentless -- OBS takes ownership only on a
		   successful add -- so leaving it here leaks the dock, every row, every weak source ref
		   and BOTH QTimers, and ff_dock_unregister cannot clean up after it either: it calls
		   obs_frontend_remove_dock on an id that was never added, which is a no-op. A 30 Hz
		   timer still firing into a .so that obs_module_unload has dlclosed is not a leak, it
		   is a crash on the way out. */
		delete g_dock;
		g_dock = nullptr;
		return;
	}

	/* Diagnostic hook, same shape as FOXFIRE_INSTALL_ZIP and FOXFIRE_PROC_PROOF in
	   plugin-main.c. The delay is not decoration: a widget grabbed before the event loop has
	   laid it out is blank, and a blank grab is exactly what the gate refuses. */
	if (const char *out = getenv("FOXFIRE_DOCK_GRAB"))
		QTimer::singleShot(3000, g_dock, [out]() { grab_to(out); });

	/* Drives a preset switch exactly as a click on the combo does -- same pending flag, same apply
	   on the next tick, same settings-and-update path. Nothing else can prove the switch works:
	   the widget is unreachable from obs-websocket and a grab cannot show that an instance
	   RELOADED. The dock logs what it asked for and what the source ended up with, and
	   tools/dock-proof.py requires the two to agree. */
	if (const char *want = getenv("FOXFIRE_DOCK_PICK"))
		QTimer::singleShot(5000, g_dock, [want]() { g_dock->pick(want, false); });
	/* The pack half. Worth its own hook because the two take DIFFERENT paths through apply():
	   a preset change writes one key, a pack change writes the pack, rebuilds the preset list
	   for it and then has to land the source on a preset that exists in the new pack. Nothing
	   gated that path, and it is the one that blanks the canvas when it goes wrong. */
	if (const char *want = getenv("FOXFIRE_DOCK_PICK_PACK"))
		QTimer::singleShot(5000, g_dock, [want]() { g_dock->pick(want, true); });
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
