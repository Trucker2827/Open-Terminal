#include "screens/kalshi/KalshiBotCockpitView.h"

#include "screens/kalshi/BotCockpitFeedHealthReader.h"
#include "screens/kalshi/BotCockpitRainLabels.h"
#include "ui/theme/Theme.h"

#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRectF>
#include <QTimer>
#include <QVector>
#include <QWheelEvent>

#include <cmath>

using namespace openmarketterminal::ui;

namespace openmarketterminal::screens::kalshi {

namespace {

// The data poll. The bot ticks once a minute and the calibrator republishes on
// its own cycle; a second is far finer than either, and it is the cadence the
// rest of the Kalshi screen already refreshes evidence at.
constexpr int kDataIntervalMs = 1'000;
// ~30 fps while something is actually moving.
constexpr int kFrameIntervalMs = 33;
// How many frames a pulse stays highlighted after it first appears.
constexpr int kFlashFrames = 45;

constexpr int kMargin = 14;
// The health-first strip (HARVEST -> THR -> 15M -> COM* -> BTC-D -> DECIDE ->
// SETTLE): a headline line and a row of coloured-dot stage chips, drawn above
// the mood banner so a dead feed cannot hide behind a ticking paper loop.
constexpr int kHealthHeadlineHeight = 16;
constexpr int kHealthStageRowHeight = 16;
constexpr int kHealthStripGap = 6;
constexpr int kHealthStripHeight = kHealthHeadlineHeight + kHealthStageRowHeight;
constexpr int kBannerHeight = 46;
constexpr int kCensusHeight = 20;
// Pinned KXBTCD (threshold) scoreboard above the flow — paper ambition family.
// Falls back to KXBTC15M only when the strike report is absent.
constexpr int kHeroHeight = 26;
constexpr int kHeroGap = 6;
constexpr int kNodeRowHeight = 104;
constexpr int kNodeRowGap = 6;
constexpr int kKpiHeight = 38;
constexpr int kStreamWidth = 300;
// Horizontal contract lanes: sticky label | L→R glyph track | status.
// Fixed compact height — do not stretch few books to fill the FLOW body; multi-
// cadence (COM-H/D, BTC-D) needs the spare rows more than padded empty lanes.
constexpr int kLaneHeight = 24;
constexpr int kMinLaneHeight = kLaneHeight; // scroll / drawable math
constexpr int kLaneLabelWidth = 96;
constexpr int kLaneStatusWidth = 72;
constexpr int kFlowAxisHeight = 16;
constexpr int kFlowScrollGutter = 10;

QColor with_alpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

/// Soft lane tint by calibrator family — threshold cyan, BTC amber,
/// commodities green — so multi-source flow is readable without reading tags.
QColor source_tint(const QString& signal_source, bool dormant) {
    if (signal_source == QLatin1String("kxbtc15m") ||
        signal_source == QLatin1String("kxbtc_daily"))
        return with_alpha(QColor(colors::WARNING()), dormant ? 10 : 22);
    if (signal_source == QLatin1String("commodities15m") ||
        signal_source == QLatin1String("commodities_hourly") ||
        signal_source == QLatin1String("commodities_daily"))
        return with_alpha(QColor(colors::GREEN()), dormant ? 8 : 18);
    if (signal_source == QLatin1String("threshold"))
        return with_alpha(QColor(colors::CYAN()), dormant ? 8 : 16);
    return QColor(0, 0, 0, 0);
}

/// A per-lane phase offset, so glyphs do not travel in lockstep. Derived from
/// the ticker, so a lane keeps its own rhythm across refreshes instead of
/// jumping when the report re-lists.
double lane_offset(const QString& ticker) {
    return static_cast<double>(qHash(ticker) % 997) / 997.0;
}

/// Signed flow glyphs (`edge`, `open`) take greenish/reddish ink by sign.
/// All glyphs still travel L→R — colour carries the sign, not direction.
bool is_signed_flow_glyph(const QString& label) {
    return label == QLatin1String("edge") || label == QLatin1String("open");
}

/// Soft greenish / reddish ink by sign. Zero is muted — not a win or a loss.
QColor signed_flow_ink(double value, bool dormant) {
    if (value > 0.0)
        return with_alpha(QColor(colors::GREEN()), dormant ? 130 : 210);
    if (value < 0.0) return with_alpha(QColor(colors::RED()), dormant ? 130 : 210);
    return QColor(colors::TEXT_SECONDARY());
}

QString elide(const QFontMetrics& metrics, const QString& text, int width) {
    return metrics.elidedText(text, Qt::ElideRight, width);
}

/// The age of the newest row in kalshi-tickers.jsonl, the harvest's fallback
/// clock when kalshi-ws-engine.json has no `last_event_at` of its own (e.g. an
/// engine that has connected but not yet ticked). Only the tail of the file is
/// read — the file grows without bound and only the newest row matters here.
qint64 newest_ticker_event_age_ms(qint64 now_ms) {
    QFile file(cli::kalshi_evidence_path(QStringLiteral("kalshi-tickers.jsonl")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() <= 0) return -1;
    constexpr qint64 kTailBytes = 64 * 1024;
    if (file.size() > kTailBytes) file.seek(file.size() - kTailBytes);
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) continue;
        const QDateTime ts = QDateTime::fromString(
            document.object().value(QStringLiteral("received_ts")).toString(), Qt::ISODateWithMs);
        if (ts.isValid()) return qMax<qint64>(0, now_ms - ts.toMSecsSinceEpoch());
    }
    return -1;
}

/// Feed/harvest health from kalshi-ws-engine.json, read at the same cadence
/// (and through the same evidence-path helper) as calibrator.json, the gate,
/// and the ledger already are in `load_bot_cockpit_scene`. Read-only: this
/// never writes an evidence file. The parsing itself (age off
/// `last_market_event_at`, never the CF-Benchmarks-conflated `last_event_at`)
/// lives in the pure, unit-tested `parse_bot_cockpit_feed_health`.
BotCockpitFeedHealth read_bot_cockpit_feed_health(qint64 now_ms) {
    QFile engine_file(cli::kalshi_evidence_path(QStringLiteral("kalshi-ws-engine.json")));
    QJsonObject engine;
    bool readable = false;
    if (engine_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument document = QJsonDocument::fromJson(engine_file.readAll());
        readable = document.isObject();
        if (readable) engine = document.object();
    }
    return parse_bot_cockpit_feed_health(engine, readable, now_ms, newest_ticker_event_age_ms(now_ms));
}

} // namespace

KalshiBotCockpitView::KalshiBotCockpitView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(880, 560);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    data_timer_ = new QTimer(this);
    data_timer_->setInterval(kDataIntervalMs);
    connect(data_timer_, &QTimer::timeout, this, &KalshiBotCockpitView::reload);

    frame_timer_ = new QTimer(this);
    frame_timer_->setInterval(kFrameIntervalMs);
    connect(frame_timer_, &QTimer::timeout, this, [this]() {
        ++frame_;
        phase_ += 1.0;
        update();
    });
}

void KalshiBotCockpitView::set_live_status_provider(std::function<QJsonObject()> provider) {
    live_status_provider_ = std::move(provider);
}

void KalshiBotCockpitView::reload() {
    const QJsonObject live_status = live_status_provider_ ? live_status_provider_() : QJsonObject();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    apply_scene(load_bot_cockpit_scene(live_status, now_ms, read_bot_cockpit_feed_health(now_ms)));
}

void KalshiBotCockpitView::apply_scene(const BotCockpitScene& scene) {
    scene_ = scene;
    if (inspect_node_id_ == QLatin1String(kBotCockpitPostmortemInspectId)) {
        if (scene_.postmortem_detail.isEmpty()) inspect_node_id_.clear();
    } else if (is_family_chip_inspect_id(inspect_node_id_)) {
        const BotCockpitFamilyChip* chip =
            scene_.family_chip(family_chip_id_from_inspect(inspect_node_id_));
        if (!chip || chip->detail.isEmpty()) inspect_node_id_.clear();
    } else if (!inspect_node_id_.isEmpty()) {
        const BotCockpitNode* open = scene_.node(inspect_node_id_);
        if (!open || open->detail.isEmpty()) inspect_node_id_.clear();
    }
    clamp_lane_scroll();
    // One flash per real data event: a key that is already known keeps its
    // birth frame, so re-reading the same ledger re-fires nothing.
    for (const auto& pulse : scene_.pulses)
        if (!pulse_age_frames_.contains(pulse.key)) pulse_age_frames_.insert(pulse.key, frame_);
    sync_animation_timer();
    update();
}

void KalshiBotCockpitView::open_postmortem_inspect() {
    if (scene_.postmortem_detail.isEmpty()) reload();
    if (scene_.postmortem_detail.isEmpty()) return;
    inspect_node_id_ = QString::fromLatin1(kBotCockpitPostmortemInspectId);
    setFocus(Qt::OtherFocusReason);
    update();
}

int KalshiBotCockpitView::postmortem_kpi_index() const {
    for (int i = 0; i < scene_.kpi.size(); ++i) {
        if (scene_.kpi.at(i).startsWith(QLatin1String("PM "))) return i;
    }
    return -1;
}

KalshiBotCockpitView::KpiStripLayout KalshiBotCockpitView::layout_kpi_strip() const {
    KpiStripLayout layout;
    if (scene_.kpi.isEmpty()) return layout;
    layout.entry_rects.resize(scene_.kpi.size());
    const QRect kpi_rect(kMargin, height() - kMargin - kKpiHeight, width() - (2 * kMargin),
                         kKpiHeight);
    QFont body_font = font();
    body_font.setPointSizeF(qMax(10.0, body_font.pointSizeF()));
    body_font.setBold(true);
    const QFontMetrics body_metrics(body_font);
    const QString kpi_separator = QStringLiteral("   ·   ");
    const int sep_w = body_metrics.horizontalAdvance(kpi_separator);
    const int pm_idx = postmortem_kpi_index();
    const int right_pad = 8;
    const int left_pad = 10;

    int right_limit = kpi_rect.right() - right_pad;
    if (pm_idx >= 0) {
        const int pm_w = body_metrics.horizontalAdvance(scene_.kpi.at(pm_idx));
        const int pm_x = qMax(kpi_rect.left() + left_pad, right_limit - pm_w);
        layout.entry_rects[pm_idx] = QRect(pm_x, kpi_rect.top(), pm_w, kpi_rect.height());
        right_limit = pm_x - sep_w;
    }

    int kpi_x = kpi_rect.left() + left_pad;
    int hidden = 0;
    for (int i = 0; i < scene_.kpi.size(); ++i) {
        if (i == pm_idx) continue;
        const QString entry = scene_.kpi.at(i);
        const int entry_width = body_metrics.horizontalAdvance(entry);
        if (kpi_x + entry_width > right_limit) {
            ++hidden;
            for (int j = i + 1; j < scene_.kpi.size(); ++j)
                if (j != pm_idx) ++hidden;
            if (hidden > 0) {
                const QString overflow = QStringLiteral("… %1 more").arg(hidden);
                const int overflow_w = body_metrics.horizontalAdvance(overflow);
                if (kpi_x + overflow_w <= right_limit) {
                    layout.overflow_rect =
                        QRect(kpi_x, kpi_rect.top(), overflow_w, kpi_rect.height());
                    layout.overflow_text = overflow;
                }
            }
            break;
        }
        layout.entry_rects[i] = QRect(kpi_x, kpi_rect.top(), entry_width, kpi_rect.height());
        kpi_x += entry_width + sep_w;
    }
    return layout;
}

QVector<QRect> KalshiBotCockpitView::kpi_entry_rects() const {
    return layout_kpi_strip().entry_rects;
}

bool KalshiBotCockpitView::postmortem_kpi_at(const QPoint& pos) const {
    const int idx = postmortem_kpi_index();
    if (idx < 0) return false;
    const QVector<QRect> rects = kpi_entry_rects();
    if (idx >= rects.size() || rects.at(idx).isEmpty()) return false;
    return rects.at(idx).contains(pos);
}

QString KalshiBotCockpitView::inspect_detail_text() const {
    if (inspect_node_id_ == QLatin1String(kBotCockpitPostmortemInspectId))
        return scene_.postmortem_detail;
    if (is_family_chip_inspect_id(inspect_node_id_)) {
        if (const BotCockpitFamilyChip* chip =
                scene_.family_chip(family_chip_id_from_inspect(inspect_node_id_)))
            return chip->detail;
        return {};
    }
    if (const BotCockpitNode* open = scene_.node(inspect_node_id_)) return open->detail;
    return {};
}

QString KalshiBotCockpitView::inspect_title() const {
    if (inspect_node_id_ == QLatin1String(kBotCockpitPostmortemInspectId))
        return QStringLiteral("BID POSTMORTEM");
    if (is_family_chip_inspect_id(inspect_node_id_)) {
        if (const BotCockpitFamilyChip* chip =
                scene_.family_chip(family_chip_id_from_inspect(inspect_node_id_)))
            return QStringLiteral("DECIDE · %1").arg(chip->label);
        return QStringLiteral("DECIDE");
    }
    if (const BotCockpitNode* open = scene_.node(inspect_node_id_)) return open->label;
    return {};
}

QVector<QRect> KalshiBotCockpitView::family_chip_hit_rects() const {
    QVector<QRect> rects;
    if (decide_body_rect_.isEmpty() || scene_.family_chips.isEmpty()) return rects;
    const int n = scene_.family_chips.size();
    constexpr int kChipH = 20;
    constexpr int kPad = 6;
    const int row_y = decide_body_rect_.bottom() - kChipH - kPad;
    const int usable = decide_body_rect_.width() - 2 * kPad;
    if (usable < n * 20 || row_y < decide_body_rect_.top() + 36) return rects;
    const int chip_w = usable / n;
    rects.reserve(n);
    for (int i = 0; i < n; ++i) {
        rects << QRect(decide_body_rect_.left() + kPad + i * chip_w, row_y, chip_w - 2, kChipH);
    }
    return rects;
}

const BotCockpitFamilyChip* KalshiBotCockpitView::family_chip_at(const QPoint& pos) const {
    const QVector<QRect> rects = family_chip_hit_rects();
    for (int i = 0; i < rects.size() && i < scene_.family_chips.size(); ++i) {
        if (rects.at(i).contains(pos) && !scene_.family_chips.at(i).detail.isEmpty())
            return &scene_.family_chips.at(i);
    }
    return nullptr;
}

int KalshiBotCockpitView::flow_lane_capacity() const {
    int h = flow_body_rect_.height();
    if (h <= 8) {
        // Before the first paint, estimate from the widget so scroll clamps
        // are sane instead of pretending only one lane fits.
        h = qMax(120, height() - 280);
    }
    return qMax(1, (h - 8) / kMinLaneHeight);
}

int KalshiBotCockpitView::max_lane_scroll() const {
    return qMax(0, scene_.columns.size() - flow_lane_capacity());
}

void KalshiBotCockpitView::clamp_lane_scroll() {
    lane_scroll_ = qBound(0, lane_scroll_, max_lane_scroll());
}

bool KalshiBotCockpitView::scroll_lanes_by(int delta_lanes) {
    if (delta_lanes == 0) return false;
    const int before = lane_scroll_;
    lane_scroll_ = qBound(0, lane_scroll_ + delta_lanes, max_lane_scroll());
    return lane_scroll_ != before;
}

void KalshiBotCockpitView::sync_animation_timer() {
    // The whole perf story in one line: nothing moving, no frames. A frozen or
    // dormant cockpit repaints only when new data arrives.
    const bool should_animate = isVisible() && scene_.motion;
    if (should_animate && !frame_timer_->isActive()) frame_timer_->start();
    if (!should_animate && frame_timer_->isActive()) frame_timer_->stop();
}

bool KalshiBotCockpitView::animating() const { return frame_timer_ && frame_timer_->isActive(); }

void KalshiBotCockpitView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    reload();
    data_timer_->start();
    sync_animation_timer();
}

void KalshiBotCockpitView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    data_timer_->stop();
    frame_timer_->stop();
}

QColor KalshiBotCockpitView::role_color(const QString& role) const {
    if (role == QStringLiteral("green")) return QColor(colors::GREEN());
    if (role == QStringLiteral("red")) return QColor(colors::RED());
    if (role == QStringLiteral("amber")) return QColor(colors::WARNING());
    if (role == QStringLiteral("cyan")) return QColor(colors::CYAN());
    return QColor(colors::TEXT_SECONDARY());
}

QColor KalshiBotCockpitView::mood_color() const {
    if (scene_.mood == QString::fromLatin1(kBotCockpitMoodLive)) return QColor(colors::RED());
    if (scene_.mood == QString::fromLatin1(kBotCockpitMoodPaper)) return QColor(colors::CYAN());
    return QColor(colors::TEXT_SECONDARY());
}

int KalshiBotCockpitView::orbit_row_count() const {
    int max_row = 0;
    for (const BotCockpitNode& node : scene_.nodes)
        max_row = qMax(max_row, node.row);
    return scene_.nodes.isEmpty() ? 0 : max_row + 1;
}

int KalshiBotCockpitView::orbit_band_height() const {
    const int rows = orbit_row_count();
    if (rows <= 0) return 0;
    return rows * kNodeRowHeight + (rows - 1) * kNodeRowGap;
}

QRect KalshiBotCockpitView::orbit_band_rect() const {
    const int band_h = orbit_band_height();
    if (band_h <= 0) return {};
    const QRect kpi_rect(kMargin, height() - kMargin - kKpiHeight, width() - (2 * kMargin),
                         kKpiHeight);
    return QRect(kMargin, kpi_rect.top() - 6 - band_h, width() - (2 * kMargin), band_h);
}

QRect KalshiBotCockpitView::orbit_row_rect(int row) const {
    const QRect band = orbit_band_rect();
    if (band.isEmpty() || row < 0 || row >= orbit_row_count()) return {};
    return QRect(band.left(), band.top() + row * (kNodeRowHeight + kNodeRowGap), band.width(),
                 kNodeRowHeight);
}

QList<int> KalshiBotCockpitView::node_indices_for_row(int row) const {
    QList<int> indices;
    for (int i = 0; i < scene_.nodes.size(); ++i) {
        if (scene_.nodes.at(i).row == row) indices.append(i);
    }
    return indices;
}

QRectF KalshiBotCockpitView::node_hit_rect(int index) const {
    if (index < 0 || index >= scene_.nodes.size() || scene_.nodes.isEmpty()) return {};
    const int row = scene_.nodes.at(index).row;
    const QList<int> peers = node_indices_for_row(row);
    const int pos_in_row = peers.indexOf(index);
    if (pos_in_row < 0 || peers.isEmpty()) return {};
    const QRect row_rect = orbit_row_rect(row);
    if (row_rect.isEmpty()) return {};
    const double node_width = static_cast<double>(row_rect.width()) / peers.size();
    return QRectF(row_rect.left() + (pos_in_row * node_width) + 3, row_rect.top(), node_width - 6,
                  row_rect.height());
}

const BotCockpitNode* KalshiBotCockpitView::node_at(const QPoint& pos) const {
    for (int i = 0; i < scene_.nodes.size(); ++i) {
        if (node_hit_rect(i).contains(pos)) return &scene_.nodes.at(i);
    }
    return nullptr;
}

void KalshiBotCockpitView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (postmortem_kpi_at(event->pos()) && !scene_.postmortem_detail.isEmpty()) {
        const QString id = QString::fromLatin1(kBotCockpitPostmortemInspectId);
        if (inspect_node_id_ == id) inspect_node_id_.clear();
        else inspect_node_id_ = id;
        setFocus(Qt::MouseFocusReason);
        update();
        event->accept();
        return;
    }
    if (const BotCockpitFamilyChip* chip = family_chip_at(event->pos())) {
        const QString id = family_chip_inspect_id(chip->id);
        if (inspect_node_id_ == id) inspect_node_id_.clear();
        else inspect_node_id_ = id;
        setFocus(Qt::MouseFocusReason);
        update();
        event->accept();
        return;
    }
    const BotCockpitNode* node = node_at(event->pos());
    if (node && !node->detail.isEmpty()) {
        if (inspect_node_id_ == node->id) inspect_node_id_.clear();
        else inspect_node_id_ = node->id;
        setFocus(Qt::MouseFocusReason);
        update();
        event->accept();
        return;
    }
    if (!inspect_node_id_.isEmpty()) {
        inspect_node_id_.clear();
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KalshiBotCockpitView::mouseMoveEvent(QMouseEvent* event) {
    const BotCockpitNode* node = node_at(event->pos());
    if ((node && !node->detail.isEmpty()) ||
        (postmortem_kpi_at(event->pos()) && !scene_.postmortem_detail.isEmpty()) ||
        family_chip_at(event->pos()) != nullptr) {
        setCursor(Qt::PointingHandCursor);
    } else if (flow_body_rect_.contains(event->pos()) && max_lane_scroll() > 0) {
        setCursor(Qt::SizeVerCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void KalshiBotCockpitView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && !inspect_node_id_.isEmpty()) {
        inspect_node_id_.clear();
        update();
        event->accept();
        return;
    }
    int delta = 0;
    switch (event->key()) {
    case Qt::Key_Down:
    case Qt::Key_J:
        delta = 1;
        break;
    case Qt::Key_Up:
    case Qt::Key_K:
        delta = -1;
        break;
    case Qt::Key_PageDown:
        delta = flow_lane_capacity();
        break;
    case Qt::Key_PageUp:
        delta = -flow_lane_capacity();
        break;
    case Qt::Key_Home:
        if (lane_scroll_ != 0) {
            lane_scroll_ = 0;
            update();
            event->accept();
            return;
        }
        break;
    case Qt::Key_End:
        if (lane_scroll_ != max_lane_scroll()) {
            lane_scroll_ = max_lane_scroll();
            update();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    if (delta != 0 && scroll_lanes_by(delta)) {
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KalshiBotCockpitView::wheelEvent(QWheelEvent* event) {
    if (!flow_body_rect_.contains(event->position().toPoint()) || scene_.columns.isEmpty()) {
        QWidget::wheelEvent(event);
        return;
    }
    // Prefer vertical scroll; trackpads report pixel deltas, mice report steps.
    int delta_lanes = 0;
    if (!event->pixelDelta().isNull()) {
        // ~one lane per min-lane height of trackpad travel.
        wheel_accum_ += event->pixelDelta().y();
        while (wheel_accum_ <= -kMinLaneHeight) {
            ++delta_lanes;
            wheel_accum_ += kMinLaneHeight;
        }
        while (wheel_accum_ >= kMinLaneHeight) {
            --delta_lanes;
            wheel_accum_ -= kMinLaneHeight;
        }
    } else {
        const int steps = event->angleDelta().y() / 120;
        delta_lanes = -steps;  // wheel up → earlier lanes
    }
    if (delta_lanes != 0 && scroll_lanes_by(delta_lanes)) {
        update();
        event->accept();
        return;
    }
    // At end of list: still accept so the parent does not steal the gesture.
    if (max_lane_scroll() > 0) {
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void KalshiBotCockpitView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor mood = mood_color();
    const bool dormant = scene_.dormant;
    // A dead cockpit looks dead: the dormant scene is drawn at a fraction of
    // the contrast of a working one, so "nothing is happening" reads across the
    // room rather than needing to be read off a label.
    painter.fillRect(rect(), QColor(colors::BG_BASE()));
    painter.fillRect(rect(), with_alpha(mood, dormant ? 6 : 14));

    QFont title_font(QStringLiteral("Menlo"), 13, QFont::Black);
    QFont body_font(QStringLiteral("Menlo"), 10);
    QFont small_font(QStringLiteral("Menlo"), 9);
    const QFontMetrics body_metrics(body_font);
    const QFontMetrics small_metrics(small_font);

    // ── health-first strip (issue: HARVEST -> CALIBRATE -> DECIDE -> SETTLE) ─
    // Above everything else, including the mood banner: this line answers "is
    // the pipeline that feeds the loop healthy", which a ticking paper loop
    // must never be able to hide behind.
    const QRect health_strip(kMargin, kMargin, width() - (2 * kMargin), kHealthStripHeight);
    {
        const QColor headline_ink = role_color(scene_.health_role);
        painter.setFont(body_font);
        painter.setPen(headline_ink);
        const QRect headline_rect(health_strip.left(), health_strip.top(),
                                  health_strip.width(), kHealthHeadlineHeight);
        painter.drawText(headline_rect, Qt::AlignVCenter | Qt::AlignLeft,
                         elide(body_metrics, scene_.health_banner, headline_rect.width()));

        const QRect stage_row(health_strip.left(), headline_rect.bottom(), health_strip.width(),
                              kHealthStageRowHeight);
        const int stage_count = static_cast<int>(scene_.health_stages.size());
        if (stage_count > 0) {
            const double stage_width = static_cast<double>(stage_row.width()) / stage_count;
            constexpr int kDotDiameter = 8;
            painter.setFont(small_font);
            for (int i = 0; i < stage_count; ++i) {
                const BotCockpitHealthStage& stage = scene_.health_stages.at(i);
                const QColor ink = role_color(stage.role);
                const double x = stage_row.left() + (i * stage_width);
                const QRectF dot(x, stage_row.top() + (stage_row.height() - kDotDiameter) / 2.0,
                                 kDotDiameter, kDotDiameter);
                painter.setPen(Qt::NoPen);
                painter.setBrush(ink);
                painter.drawEllipse(dot);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(ink);
                const QRectF label(dot.right() + 4, stage_row.top(),
                                   stage_width - (dot.width() + 8), stage_row.height());
                painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft,
                                 elide(small_metrics,
                                       QStringLiteral("%1 %2").arg(stage.label, stage.value),
                                       qMax(0, static_cast<int>(label.width()))));
            }
        }
    }

    // ── banner ─────────────────────────────────────────────────────────────
    const QRect banner(kMargin, health_strip.bottom() + kHealthStripGap,
                       width() - (2 * kMargin), kBannerHeight);
    painter.fillRect(banner, with_alpha(mood, dormant ? 20 : 46));
    painter.setPen(QPen(mood, scene_.live ? 3 : 1));
    painter.drawRect(banner);
    painter.setFont(title_font);
    painter.setPen(dormant ? QColor(colors::TEXT_SECONDARY()) : mood);
    painter.drawText(banner.adjusted(12, 4, -12, -22), Qt::AlignVCenter | Qt::AlignLeft,
                     elide(QFontMetrics(title_font), scene_.banner, banner.width() - 24));
    painter.setFont(small_font);
    painter.setPen(QColor(colors::TEXT_SECONDARY()));
    painter.drawText(banner.adjusted(12, 24, -12, -4), Qt::AlignVCenter | Qt::AlignLeft,
                     elide(small_metrics, scene_.mood_reason, banner.width() - 24));

    // ── census (what the L→R flow is, and what it is not showing) ──────────
    const QRect census(kMargin, banner.bottom() + 4, width() - (2 * kMargin), kCensusHeight);
    painter.setFont(small_font);
    painter.setPen(QColor(scene_.columns_frozen > 0 ? colors::WARNING()
                                                    : colors::TEXT_SECONDARY()));
    painter.drawText(census, Qt::AlignVCenter | Qt::AlignLeft,
                     elide(small_metrics, scene_.census, census.width()));

    // ── strategy-grid advisory ─────────────────────────────────────────────
    // One read-only line from the paper strategy-grid, beside the gate — never
    // driving it. Cyan when a survivor/candidate is named, muted for "no
    // measured edge" / UNAVAILABLE / STALE. The line is the presenter's;
    // tst_kalshi_bot_cockpit and tst_kalshi_strategy_grid_view hold it to account.
    const QRect grid_rect(kMargin, census.bottom() + 4, width() - (2 * kMargin),
                          small_metrics.lineSpacing() + 4);
    if (!scene_.grid_line.isEmpty()) {
        const bool has_signal = scene_.grid_line.contains(QStringLiteral("forming")) ||
                                scene_.grid_line.contains(QStringLiteral("ADVISORY"));
        painter.setFont(small_font);
        painter.setPen(QColor(has_signal ? colors::CYAN() : colors::TEXT_SECONDARY()));
        painter.drawText(grid_rect, Qt::AlignVCenter | Qt::AlignLeft,
                         elide(small_metrics, scene_.grid_line, grid_rect.width()));
    }

    // ── WHAT THE RECORD TEACHES (issue #174) ───────────────────────────────
    // The autopsy's standing conclusions, above the flow: what the record has
    // already taught frames what the bot is doing right now. Every line is the
    // presenter's — the same text the BOT tab and `kalshi bot lessons` show,
    // sample size included — and its colour is the presenter's role, so a
    // stale artifact arrives here with its greens already demoted to amber.
    const int lesson_line_height = small_metrics.lineSpacing() + 2;
    const int lessons_lines = static_cast<int>(scene_.lessons.size());
    const QRect lessons_rect(kMargin, grid_rect.bottom() + 6, width() - (2 * kMargin),
                             lessons_lines > 0 ? (lessons_lines * lesson_line_height) + 10 : 0);
    if (lessons_lines > 0) {
        painter.fillRect(lessons_rect, with_alpha(QColor(colors::BG_RAISED()), dormant ? 70 : 120));
        painter.setPen(QPen(with_alpha(QColor(scene_.lessons_stale ? colors::WARNING()
                                                                   : colors::CYAN()), 110), 1));
        painter.drawRect(lessons_rect);
        painter.setFont(small_font);
        for (int i = 0; i < lessons_lines; ++i) {
            const QString role = i < scene_.lessons_roles.size() ? scene_.lessons_roles.at(i)
                                                                 : QString();
            painter.setPen(role_color(role));
            const QRect line(lessons_rect.left() + 8,
                             lessons_rect.top() + 5 + (i * lesson_line_height),
                             lessons_rect.width() - 16, lesson_line_height);
            // The full line when it fits, the presenter's compact form when it
            // does not. Eliding the full line cuts from the RIGHT, which is
            // where the sample size sits — a conclusion drawn here without its
            // denominator is the one thing this card must never show. Both
            // strings come from the presenter; nothing is composed here.
            const QString full = scene_.lessons.at(i);
            const QString compact = i < scene_.lessons_compact.size()
                                        ? scene_.lessons_compact.at(i) : full;
            const QString drawn =
                small_metrics.horizontalAdvance(full) <= line.width() ? full : compact;
            painter.drawText(line, Qt::AlignVCenter | Qt::AlignLeft,
                             elide(small_metrics, drawn, line.width()));
        }
    }

    // ── KXBTCD / threshold scoreboard hero (pinned above the flow) ─────────
    // Paper ambition: strike books lead. Progress to the trust floor + the same
    // sentence as the CALIBRATOR orbit node. Falls back to KXBTC15M only when
    // calibrator.json is absent so the pin is never empty for no reason.
    int upper_bottom = census.bottom();
    if (!scene_.grid_line.isEmpty()) upper_bottom = grid_rect.bottom();
    if (lessons_lines > 0) upper_bottom = lessons_rect.bottom();
    const int content_after_upper = upper_bottom + 6;
    const bool use_threshold_hero = !scene_.threshold_hero_line.isEmpty();
    const QString hero_line =
        use_threshold_hero ? scene_.threshold_hero_line : scene_.kxbtc15m_hero_line;
    const QString hero_role =
        use_threshold_hero ? scene_.threshold_hero_role : scene_.kxbtc15m_hero_role;
    const int hero_scored =
        use_threshold_hero ? scene_.threshold_hero_scored : scene_.kxbtc15m_hero_scored;
    const int hero_floor =
        use_threshold_hero ? scene_.threshold_hero_floor : scene_.kxbtc15m_hero_floor;
    const bool show_hero = !hero_line.isEmpty();
    const QRect hero_rect(kMargin, content_after_upper, width() - (2 * kMargin),
                          show_hero ? kHeroHeight : 0);
    if (show_hero) {
        const QColor ink = role_color(hero_role);
        painter.fillRect(hero_rect, with_alpha(QColor(colors::BG_RAISED()), dormant ? 70 : 130));
        painter.setPen(QPen(with_alpha(ink, 160), 1));
        painter.drawRect(hero_rect);
        const int bar_left = hero_rect.left() + 8;
        const int bar_width = qMax(40, hero_rect.width() / 5);
        const int bar_top = hero_rect.top() + 8;
        const int bar_height = hero_rect.height() - 16;
        const QRect track(bar_left, bar_top, bar_width, bar_height);
        painter.fillRect(track, with_alpha(QColor(colors::BG_BASE()), 180));
        const double floor = qMax(1, hero_floor);
        const double fill =
            qBound(0.0, static_cast<double>(hero_scored) / floor, 1.0);
        painter.fillRect(QRect(track.left(), track.top(),
                               static_cast<int>(track.width() * fill), track.height()),
                         with_alpha(ink, dormant ? 90 : 180));
        painter.setFont(small_font);
        painter.setPen(ink);
        const QRect text_rect(track.right() + 10, hero_rect.top(),
                              hero_rect.right() - track.right() - 18, hero_rect.height());
        painter.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft,
                         elide(small_metrics, hero_line, text_rect.width()));
    }

    // ── layout of the lower furniture ──────────────────────────────────────
    const QRect kpi_rect(kMargin, height() - kMargin - kKpiHeight, width() - (2 * kMargin),
                         kKpiHeight);
    const QRect node_rect = orbit_band_rect();
    const int field_top =
        (show_hero ? hero_rect.bottom() + kHeroGap : content_after_upper);
    QRect field(kMargin, field_top, width() - (2 * kMargin),
                (node_rect.isEmpty() ? kpi_rect.top() : node_rect.top()) - field_top - 6);
    if (field.height() < 120) field.setHeight(120);

    // ── L→R pipeline: FLOW lanes → DECIDE → LEDGER ─────────────────────────
    const int stream_width = field.width() > (kStreamWidth * 2) ? kStreamWidth : 0;
    const int decide_width = qBound(160, field.width() / 5, 220);
    const QRect stream(field.right() - stream_width + 1, field.top(), stream_width,
                       field.height());
    const QRect decide(stream_width > 0 ? stream.left() - 10 - decide_width
                                        : field.right() - decide_width + 1,
                       field.top(), decide_width, field.height());
    const QRect flow(field.left(), field.top(),
                     qMax(120, decide.left() - field.left() - 10), field.height());

    const QRect flow_body(flow.left(), flow.top() + kFlowAxisHeight + 2, flow.width(),
                          flow.height() - kFlowAxisHeight - 4);
    flow_body_rect_ = flow_body;
    clamp_lane_scroll();

    const int drawable = qMax(1, (flow_body.height() - 8) / kMinLaneHeight);
    const int total_lanes = scene_.columns.size();
    const int max_scroll = qMax(0, total_lanes - drawable);
    const int drawn = qMin(drawable, total_lanes);
    const bool scrollable = max_scroll > 0;

    // Axis caption reinforces the reading direction (+ scroll hint when needed).
    painter.setFont(small_font);
    painter.setPen(QColor(colors::TEXT_SECONDARY()));
    const QString flow_caption =
        scrollable ? QStringLiteral("FLOW  →  · scroll for more") : QStringLiteral("FLOW  →");
    painter.drawText(QRect(flow.left() + 8, flow.top() + 2, flow.width() - 16, kFlowAxisHeight),
                     Qt::AlignVCenter | Qt::AlignLeft, flow_caption);
    painter.drawText(QRect(decide.left() + 6, decide.top() + 2, decide.width() - 12,
                           kFlowAxisHeight),
                     Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("DECIDE"));
    if (stream_width > 0) {
        painter.drawText(QRect(stream.left() + 8, stream.top() + 2, stream.width() - 16,
                               kFlowAxisHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("LEDGER  →"));
    }

    painter.fillRect(flow_body, with_alpha(QColor(colors::BG_RAISED()), dormant ? 60 : 110));
    painter.setPen(QPen(with_alpha(mood, 70), 1));
    painter.drawRect(flow_body);

    if (scene_.columns.isEmpty()) {
        painter.setFont(body_font);
        painter.setPen(QColor(colors::TEXT_SECONDARY()));
        painter.drawText(flow_body, Qt::AlignCenter,
                         QStringLiteral("NO FLOW\nwaiting for next open contract\n"
                                        "(closed 15m windows are omitted)"));
    } else {
        const int gutter = scrollable ? kFlowScrollGutter : 0;
        // Cap at kLaneHeight so 3–4 open books stay slim; spare FLOW height is
        // reserved for commodities / other cadences as they appear (or scroll).
        const double lane_height = static_cast<double>(kLaneHeight);
        for (int i = 0; i < drawn; ++i) {
            const BotCockpitColumn& column = scene_.columns.at(lane_scroll_ + i);
            const QRectF lane(flow_body.left() + 4, flow_body.top() + 4 + (i * lane_height),
                              flow_body.width() - 8 - gutter, lane_height - 2);
            const QColor ignition = column.ignition_side == QStringLiteral("NO")
                                        ? QColor(colors::RED())
                                        : QColor(colors::GREEN());
            const QString source_tag = bot_cockpit_source_tag(column.signal_source);

            const QColor tint = source_tint(column.signal_source, dormant);
            if (tint.alpha() > 0) painter.fillRect(lane, tint);
            if (column.ignitions > 0)
                painter.fillRect(lane, with_alpha(ignition, dormant ? 18 : 34));
            if (column.settled)
                painter.fillRect(lane,
                                 with_alpha(column.settled_won ? QColor(colors::GREEN())
                                                               : QColor(colors::RED()),
                                            dormant ? 14 : 26));

            // Sticky left label: family tag + close time / strike.
            const QRectF label_box(lane.left(), lane.top(), kLaneLabelWidth, lane.height());
            painter.setFont(small_font);
            painter.setPen(column.frozen ? QColor(colors::WARNING())
                           : dormant     ? QColor(colors::TEXT_SECONDARY())
                                         : mood);
            const QString head = bot_cockpit_column_head(column.ticker);
            const QString headed =
                source_tag.isEmpty() ? head : QStringLiteral("%1 %2").arg(source_tag, head);
            painter.drawText(label_box.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             elide(small_metrics, headed, static_cast<int>(label_box.width()) - 6));

            // Sticky right status: FROZEN / BID / settle outcome.
            const QRectF status_box(lane.right() - kLaneStatusWidth, lane.top(), kLaneStatusWidth,
                                    lane.height());
            if (column.settled) {
                painter.setPen(column.settled_won ? QColor(colors::GREEN())
                                                  : QColor(colors::RED()));
                painter.drawText(
                    status_box.adjusted(2, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight,
                    elide(small_metrics,
                          QStringLiteral("%1 $%2")
                              .arg(column.settled_won ? QStringLiteral("WON")
                                                     : QStringLiteral("LOST"))
                              .arg(column.settled_pnl_usd, 0, 'f', 2),
                          static_cast<int>(status_box.width()) - 6));
            } else if (column.ignitions > 0) {
                painter.setPen(ignition);
                painter.drawText(status_box.adjusted(2, 0, -4, 0),
                                 Qt::AlignVCenter | Qt::AlignRight,
                                 elide(small_metrics,
                                       QStringLiteral("%1×%2")
                                           .arg(column.ignition_side.isEmpty()
                                                    ? QStringLiteral("BID")
                                                    : column.ignition_side)
                                           .arg(column.ignitions),
                                       static_cast<int>(status_box.width()) - 6));
            } else if (column.frozen) {
                painter.setPen(QColor(colors::WARNING()));
                painter.drawText(status_box.adjusted(2, 0, -4, 0),
                                 Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("FROZEN"));
            }

            // Glyph track: everything travels L→R. Signed edge/open use
            // greenish / reddish by sign; mid/p/sigma stay mood ink.
            // Frozen = parked (no travel), amber.
            const double track_left = label_box.right() + 4;
            const double track_right = status_box.left() - 4;
            const double track_width = qMax(40.0, track_right - track_left);
            const double track_y = lane.center().y() - 6;
            painter.setPen(QPen(with_alpha(mood, 28), 1));
            painter.drawLine(QPointF(track_left, lane.center().y()),
                             QPointF(track_right, lane.center().y()));

            const double offset = lane_offset(column.ticker);
            const double travel = column.frozen ? 0.0 : (phase_ * 2.4);
            const int glyph_count = qMax(1, column.glyphs.size());
            for (int g = 0; g < column.glyphs.size(); ++g) {
                const BotCockpitGlyph& glyph = column.glyphs.at(g);
                const bool signed_glyph =
                    glyph.known && is_signed_flow_glyph(glyph.label);
                const double span = track_width;
                const double raw =
                    std::fmod((offset * span) + travel + (g * span / glyph_count), span);
                const double pos = raw < 0 ? raw + span : raw;
                const double x = track_left + pos;
                // Brighter toward the leading (right) edge of L→R travel.
                const int alpha =
                    column.frozen ? 150 : 80 + static_cast<int>(160.0 * (pos / span));
                QColor ink;
                if (!glyph.known) {
                    ink = QColor(colors::TEXT_SECONDARY());
                } else if (column.frozen) {
                    ink = QColor(colors::WARNING());
                } else if (signed_glyph) {
                    ink = signed_flow_ink(glyph.value, dormant);
                } else if (column.ignitions > 0) {
                    ink = ignition;
                } else if (dormant) {
                    ink = QColor(colors::TEXT_SECONDARY());
                } else {
                    ink = mood;
                }
                painter.setPen(with_alpha(ink, qBound(40, alpha, 255)));
                painter.setFont(small_font);
                const QString text = QStringLiteral("%1 %2").arg(glyph.label, glyph.text);
                const int text_w = small_metrics.horizontalAdvance(text) + 4;
                painter.drawText(QRectF(x - (text_w / 2.0), track_y, text_w, 14),
                                 Qt::AlignHCenter | Qt::AlignVCenter, text);
            }
        }

        if (scrollable) {
            // Thin scrollbar on the FLOW gutter — position says where you are.
            const QRect track(flow_body.right() - gutter + 1, flow_body.top() + 4, gutter - 3,
                              flow_body.height() - 8);
            painter.fillRect(track, with_alpha(QColor(colors::BG_BASE()), 140));
            const double thumb_h =
                qMax(18.0, track.height() * (static_cast<double>(drawn) / total_lanes));
            const double thumb_y =
                track.top() +
                (track.height() - thumb_h) * (static_cast<double>(lane_scroll_) / max_scroll);
            painter.fillRect(QRectF(track.left(), thumb_y, track.width(), thumb_h),
                             with_alpha(mood, dormant ? 90 : 160));

            painter.setFont(small_font);
            painter.setPen(QColor(colors::WARNING()));
            const int from = lane_scroll_ + 1;
            const int to = lane_scroll_ + drawn;
            painter.drawText(flow_body.adjusted(6, 0, -(gutter + 6), -2),
                             Qt::AlignBottom | Qt::AlignRight,
                             QStringLiteral("lanes %1–%2 of %3 · scroll")
                                 .arg(from)
                                 .arg(to)
                                 .arg(total_lanes));
        }
    }

    // ── DECIDE station (right of flow, before ledger) ──────────────────────
    const QRect decide_body(decide.left(), decide.top() + kFlowAxisHeight + 2, decide.width(),
                            decide.height() - kFlowAxisHeight - 4);
    decide_body_rect_ = decide_body;
    const QColor envelope_ink = role_color(scene_.envelope_role);
    painter.fillRect(decide_body, with_alpha(QColor(colors::BG_BASE()), dormant ? 200 : 232));
    painter.setPen(QPen(envelope_ink, 2));
    painter.drawRect(decide_body);
    painter.setFont(small_font);
    painter.setPen(QColor(colors::TEXT_SECONDARY()));
    painter.drawText(decide_body.adjusted(8, 6, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                     elide(small_metrics,
                           scene_.envelope_ticker.isEmpty()
                               ? QStringLiteral("DECISION")
                               : QStringLiteral("DECISION · %1").arg(scene_.envelope_ticker),
                           decide_body.width() - 16));
    const QVector<QRect> chip_rects = family_chip_hit_rects();
    const int envelope_bottom_pad = chip_rects.isEmpty() ? 8 : 34;
    painter.setFont(body_font);
    painter.setPen(envelope_ink);
    painter.drawText(decide_body.adjusted(8, 28, -8, -envelope_bottom_pad),
                     Qt::TextWordWrap | Qt::AlignTop | Qt::AlignLeft, scene_.envelope);
    // Per-family chips under the envelope — newest overall stays above; chips
    // keep metals from inheriting each other's latest action.
    if (!chip_rects.isEmpty()) {
        painter.setFont(small_font);
        for (int i = 0; i < chip_rects.size() && i < scene_.family_chips.size(); ++i) {
            const BotCockpitFamilyChip& chip = scene_.family_chips.at(i);
            const QRect box = chip_rects.at(i);
            const QColor ink = role_color(chip.role);
            painter.fillRect(box, with_alpha(ink, dormant ? 18 : 36));
            painter.setPen(QPen(with_alpha(ink, chip.detail.isEmpty() ? 90 : 180), 1));
            painter.drawRect(box);
            if (inspect_node_id_ == family_chip_inspect_id(chip.id)) {
                painter.setPen(QPen(ink, 2));
                painter.drawRect(box.adjusted(1, 1, -1, -1));
            }
            painter.setPen(ink);
            painter.drawText(box.adjusted(2, 0, -2, 0), Qt::AlignCenter,
                             elide(small_metrics,
                                   QStringLiteral("%1 %2").arg(chip.label, chip.action),
                                   box.width() - 4));
        }
    }

    // ── LEDGER (end of the L→R pipeline) ───────────────────────────────────
    if (stream_width > 0) {
        const QRect stream_body(stream.left(), stream.top() + kFlowAxisHeight + 2, stream.width(),
                                stream.height() - kFlowAxisHeight - 4);
        painter.fillRect(stream_body, with_alpha(QColor(colors::BG_RAISED()), dormant ? 60 : 110));
        painter.setPen(QPen(with_alpha(mood, 70), 1));
        painter.drawRect(stream_body);
        painter.setFont(small_font);
        painter.setPen(QColor(colors::TEXT_SECONDARY()));
        painter.drawText(stream_body.adjusted(8, 4, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("one card per journaled event"));
        int y = stream_body.top() + 22;
        for (const auto& pulse : scene_.pulses) {
            if (y + 30 > stream_body.bottom()) break;
            const QRect card(stream_body.left() + 6, y, stream_body.width() - 12, 28);
            const QColor ink = role_color(pulse.role);
            const int age = frame_ - pulse_age_frames_.value(pulse.key, frame_ - kFlashFrames);
            const bool fresh = age < kFlashFrames;
            painter.fillRect(card, with_alpha(ink, fresh && !dormant ? 70 : 24));
            painter.setPen(QPen(with_alpha(ink, fresh ? 220 : 110), 1));
            painter.drawRect(card);
            painter.setPen(ink);
            painter.setFont(small_font);
            painter.drawText(card.adjusted(6, 0, -6, -13), Qt::AlignVCenter | Qt::AlignLeft,
                             pulse.kind.toUpper());
            painter.setPen(QColor(colors::TEXT_PRIMARY()));
            painter.drawText(card.adjusted(6, 12, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             elide(small_metrics, pulse.text, card.width() - 12));
            y += 32;
        }
        if (scene_.pulses.isEmpty()) {
            painter.setPen(QColor(colors::TEXT_SECONDARY()));
            painter.drawText(stream_body.adjusted(8, 28, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                             QStringLiteral("no journaled event in the ledger window"));
        }
    }

    // ── orbit nodes (row 0 = BTC+session; row 1 = GOLD/SILVER/WTI) ──────────
    if (!scene_.nodes.isEmpty() && !node_rect.isEmpty()) {
        for (int i = 0; i < scene_.nodes.size(); ++i) {
            const BotCockpitNode& node = scene_.nodes.at(i);
            const QRectF box = node_hit_rect(i);
            if (box.isEmpty()) continue;
            const QColor ink = role_color(node.role);
            painter.fillRect(box, with_alpha(QColor(colors::BG_RAISED()), dormant ? 70 : 130));
            painter.setPen(QPen(with_alpha(ink, node.known ? 200 : 90), node.known ? 2 : 1));
            painter.drawRect(box);
            painter.setFont(small_font);
            painter.setPen(ink);
            painter.drawText(box.adjusted(8, 4, -8, -(box.height() - 20)),
                             Qt::AlignLeft | Qt::AlignVCenter, node.label);
            painter.setPen(node.known ? QColor(colors::TEXT_PRIMARY())
                                      : QColor(colors::TEXT_SECONDARY()));
            painter.drawText(box.adjusted(8, 22, -8, -6),
                             Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, node.value);
            if (!node.detail.isEmpty() && inspect_node_id_ == node.id) {
                painter.setPen(QPen(ink, 2));
                painter.drawRect(box.adjusted(1, 1, -1, -1));
            }
        }
    }

    // ── inspect overlay (outside-info nodes or PM postmortem; never arms) ──
    if (!inspect_node_id_.isEmpty()) {
        const QString detail = inspect_detail_text();
        const QString title = inspect_title();
        if (!detail.isEmpty()) {
            const bool postmortem =
                inspect_node_id_ == QLatin1String(kBotCockpitPostmortemInspectId);
            const int panel_w = qMin(postmortem ? 720 : 560, width() - 48);
            const int panel_h = qMin(postmortem ? 480 : 320, height() - 80);
            const QRect panel((width() - panel_w) / 2, (height() - panel_h) / 2, panel_w, panel_h);
            painter.fillRect(rect(), with_alpha(QColor(colors::BG_BASE()), 140));
            painter.fillRect(panel, with_alpha(QColor(colors::BG_RAISED()), 245));
            QColor ink = QColor(colors::CYAN());
            if (postmortem) {
                if (!scene_.kpi_roles.isEmpty() && postmortem_kpi_index() >= 0 &&
                    postmortem_kpi_index() < scene_.kpi_roles.size())
                    ink = role_color(scene_.kpi_roles.at(postmortem_kpi_index()));
            } else if (is_family_chip_inspect_id(inspect_node_id_)) {
                if (const BotCockpitFamilyChip* chip =
                        scene_.family_chip(family_chip_id_from_inspect(inspect_node_id_)))
                    ink = role_color(chip->role);
            } else if (const BotCockpitNode* open = scene_.node(inspect_node_id_)) {
                ink = role_color(open->role);
            }
            painter.setPen(QPen(ink, 2));
            painter.drawRect(panel);
            painter.setFont(small_font);
            painter.setPen(ink);
            painter.drawText(panel.adjusted(12, 8, -12, -(panel.height() - 26)),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elide(small_metrics, title, panel.width() - 24));
            painter.setPen(QColor(colors::TEXT_SECONDARY()));
            painter.drawText(panel.adjusted(12, 8, -12, -(panel.height() - 26)),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Esc / click to close · inspect only"));
            painter.setFont(body_font);
            painter.setPen(QColor(colors::TEXT_PRIMARY()));
            painter.drawText(panel.adjusted(12, 30, -12, -12),
                             Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, detail);
        }
    }

    // ── KPI strip ──────────────────────────────────────────────────────────
    painter.fillRect(kpi_rect, with_alpha(QColor(colors::BG_RAISED()), dormant ? 70 : 130));
    painter.setPen(QPen(with_alpha(mood, 90), 1));
    painter.drawRect(kpi_rect);
    painter.setFont(body_font);
    // The strip is either the gate's numbers or one stated absence — it never
    // mixes a real number with a filled-in zero. Each entry is painted in the
    // role the MODEL assigned it (net P&L green/red by sign, a drawdown past
    // its sealed cap red); the widget picks no colour of its own.
    const QColor kpi_default = scene_.kpi_available ? QColor(colors::TEXT_PRIMARY())
                                                    : QColor(colors::TEXT_SECONDARY());
    const QString kpi_separator = QStringLiteral("   ·   ");
    const KpiStripLayout strip = layout_kpi_strip();
    const int pm_idx = postmortem_kpi_index();
    for (int i = 0; i < scene_.kpi.size(); ++i) {
        const QRect entry_rect = i < strip.entry_rects.size() ? strip.entry_rects.at(i) : QRect();
        if (entry_rect.isEmpty()) continue;
        const QString entry = scene_.kpi.at(i);
        const QString role = i < scene_.kpi_roles.size() ? scene_.kpi_roles.at(i) : QString();
        painter.setPen(role.isEmpty() || role == QStringLiteral("grey") ? kpi_default
                                                                       : role_color(role));
        QFont entry_font = body_font;
        if (i == pm_idx) {
            // Click affordance: underline PM so bid-postmortem inspect is discoverable.
            entry_font.setUnderline(true);
        }
        painter.setFont(entry_font);
        painter.drawText(entry_rect, Qt::AlignVCenter | Qt::AlignLeft, entry);
        // Separator between this entry and the next visible one to its right.
        int next = -1;
        for (int j = i + 1; j < scene_.kpi.size(); ++j) {
            if (j < strip.entry_rects.size() && !strip.entry_rects.at(j).isEmpty()) {
                next = j;
                break;
            }
        }
        if (next >= 0) {
            const QRect next_rect = strip.entry_rects.at(next);
            if (entry_rect.right() + 4 < next_rect.left()) {
                painter.setFont(body_font);
                painter.setPen(QColor(colors::TEXT_SECONDARY()));
                painter.drawText(QRect(entry_rect.right(), kpi_rect.top(),
                                       next_rect.left() - entry_rect.right(), kpi_rect.height()),
                                 Qt::AlignVCenter | Qt::AlignHCenter, kpi_separator.trimmed());
            }
        }
    }
    if (!strip.overflow_rect.isEmpty() && !strip.overflow_text.isEmpty()) {
        painter.setFont(body_font);
        painter.setPen(QColor(colors::WARNING()));
        painter.drawText(strip.overflow_rect, Qt::AlignVCenter | Qt::AlignLeft, strip.overflow_text);
    }
    painter.setFont(body_font);

}

} // namespace openmarketterminal::screens::kalshi
