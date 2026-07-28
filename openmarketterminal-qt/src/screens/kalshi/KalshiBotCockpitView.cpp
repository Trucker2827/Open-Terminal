#include "screens/kalshi/KalshiBotCockpitView.h"

#include "ui/theme/Theme.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRectF>
#include <QTimer>

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
constexpr int kBannerHeight = 46;
constexpr int kCensusHeight = 20;
constexpr int kNodeRowHeight = 104;
constexpr int kKpiHeight = 38;
constexpr int kStreamWidth = 330;
// A column narrower than this cannot carry a ticker and four glyphs legibly.
constexpr int kMinColumnWidth = 58;

QColor with_alpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

/// A per-column phase offset, so the columns do not fall in lockstep. Derived
/// from the ticker, so a column keeps its own rhythm across refreshes instead
/// of jumping when the report re-lists.
double column_offset(const QString& ticker) {
    return static_cast<double>(qHash(ticker) % 997) / 997.0;
}

QString elide(const QFontMetrics& metrics, const QString& text, int width) {
    return metrics.elidedText(text, Qt::ElideRight, width);
}

/// The head of a column: enough of the ticker to identify the contract in a
/// column that is only a few characters wide. `KXBTCD-26JUL2521-T64299.99`
/// identifies itself by its strike, but `KXBTC15M-26JUL230800-00` ends in a
/// bare `00`, so the last segment alone would label two different columns the
/// same. A second segment is taken whenever the last one is that short.
QString column_head(const QString& ticker) {
    const QString last = ticker.section(QLatin1Char('-'), -1);
    if (last.isEmpty()) return ticker;
    if (last.size() >= 4) return last;
    const QString previous = ticker.section(QLatin1Char('-'), -2, -2);
    return previous.isEmpty() ? last : previous + QLatin1Char('-') + last;
}

} // namespace

KalshiBotCockpitView::KalshiBotCockpitView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(880, 560);
    setAutoFillBackground(true);

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
    apply_scene(load_bot_cockpit_scene(live_status, QDateTime::currentMSecsSinceEpoch()));
}

void KalshiBotCockpitView::apply_scene(const BotCockpitScene& scene) {
    scene_ = scene;
    // One flash per real data event: a key that is already known keeps its
    // birth frame, so re-reading the same ledger re-fires nothing.
    for (const auto& pulse : scene_.pulses)
        if (!pulse_age_frames_.contains(pulse.key)) pulse_age_frames_.insert(pulse.key, frame_);
    sync_animation_timer();
    update();
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

    // ── banner ─────────────────────────────────────────────────────────────
    const QRect banner(kMargin, kMargin, width() - (2 * kMargin), kBannerHeight);
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

    // ── census (what the rain is, and what it is not showing) ──────────────
    const QRect census(kMargin, banner.bottom() + 4, width() - (2 * kMargin), kCensusHeight);
    painter.setFont(small_font);
    painter.setPen(QColor(scene_.columns_frozen > 0 ? colors::WARNING()
                                                    : colors::TEXT_SECONDARY()));
    painter.drawText(census, Qt::AlignVCenter | Qt::AlignLeft,
                     elide(small_metrics, scene_.census, census.width()));

    // ── WHAT THE RECORD TEACHES (issue #174) ───────────────────────────────
    // The autopsy's standing conclusions, above the rain: what the record has
    // already taught frames what the bot is doing right now. Every line is the
    // presenter's — the same text the BOT tab and `kalshi bot lessons` show,
    // sample size included — and its colour is the presenter's role, so a
    // stale artifact arrives here with its greens already demoted to amber.
    const int lesson_line_height = small_metrics.lineSpacing() + 2;
    const int lessons_lines = static_cast<int>(scene_.lessons.size());
    const QRect lessons_rect(kMargin, census.bottom() + 6, width() - (2 * kMargin),
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

    // ── layout of the lower furniture ──────────────────────────────────────
    const QRect kpi_rect(kMargin, height() - kMargin - kKpiHeight, width() - (2 * kMargin),
                         kKpiHeight);
    const QRect node_rect(kMargin, kpi_rect.top() - 6 - kNodeRowHeight, width() - (2 * kMargin),
                          kNodeRowHeight);
    const int field_top = (lessons_lines > 0 ? lessons_rect.bottom() : census.bottom()) + 6;
    QRect field(kMargin, field_top, width() - (2 * kMargin), node_rect.top() - field_top - 6);
    if (field.height() < 120) field.setHeight(120);

    // ── the ledger stream, on the right of the field ───────────────────────
    const int stream_width = field.width() > (kStreamWidth * 2) ? kStreamWidth : 0;
    const QRect stream(field.right() - stream_width + 1, field.top(), stream_width,
                       field.height());
    const QRect rain(field.left(), field.top(),
                     field.width() - (stream_width > 0 ? stream_width + 10 : 0), field.height());

    // ── decision rain ──────────────────────────────────────────────────────
    painter.fillRect(rain, with_alpha(QColor(colors::BG_RAISED()), dormant ? 60 : 110));
    painter.setPen(QPen(with_alpha(mood, 70), 1));
    painter.drawRect(rain);

    const int drawable = qMax(1, (rain.width() - 8) / kMinColumnWidth);
    const int drawn = qMin(drawable, static_cast<int>(scene_.columns.size()));
    if (scene_.columns.isEmpty()) {
        painter.setFont(body_font);
        painter.setPen(QColor(colors::TEXT_SECONDARY()));
        painter.drawText(rain, Qt::AlignCenter,
                         QStringLiteral("NO RAIN\nthe calibrator has published no contract for "
                                        "this cockpit to render"));
    } else {
        const double column_width = static_cast<double>(rain.width() - 8) / drawn;
        const int header_height = 30;
        const int track_top = rain.top() + header_height + 4;
        const int track_height = qMax(40, rain.height() - header_height - 12);

        for (int i = 0; i < drawn; ++i) {
            const BotCockpitColumn& column = scene_.columns.at(i);
            const QRectF cell(rain.left() + 4 + (i * column_width), rain.top(), column_width,
                              rain.height());
            const QColor ignition = column.ignition_side == QStringLiteral("NO")
                                        ? QColor(colors::RED())
                                        : QColor(colors::GREEN());

            // An ignited column burns behind its glyphs — one ignition per
            // journal row, so the count is printed rather than implied.
            if (column.ignitions > 0)
                painter.fillRect(cell, with_alpha(ignition, dormant ? 18 : 34));
            if (column.settled)
                painter.fillRect(cell,
                                 with_alpha(column.settled_won ? QColor(colors::GREEN())
                                                               : QColor(colors::RED()),
                                            dormant ? 14 : 26));

            // Header: the ticker this column IS.
            painter.setFont(small_font);
            painter.setPen(column.frozen ? QColor(colors::WARNING())
                           : dormant     ? QColor(colors::TEXT_SECONDARY())
                                         : mood);
            painter.drawText(QRectF(cell.left(), cell.top() + 2, cell.width(), 14),
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             elide(small_metrics, column_head(column.ticker),
                                   static_cast<int>(cell.width()) - 2));
            if (column.ignitions > 0) {
                painter.setPen(ignition);
                painter.drawText(QRectF(cell.left(), cell.top() + 15, cell.width(), 13),
                                 Qt::AlignHCenter | Qt::AlignVCenter,
                                 QStringLiteral("%1 x%2")
                                     .arg(column.ignition_side.isEmpty()
                                              ? QStringLiteral("BID")
                                              : column.ignition_side)
                                     .arg(column.ignitions));
            } else if (column.frozen) {
                painter.setPen(QColor(colors::WARNING()));
                painter.drawText(QRectF(cell.left(), cell.top() + 15, cell.width(), 13),
                                 Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("FROZEN"));
            }

            // The glyphs. A frozen column's glyphs sit still — the phase is not
            // applied at all, so stale data is visibly stopped, not slowed.
            const double offset = column_offset(column.ticker);
            const double travel = column.frozen ? 0.0 : (phase_ * 1.6);
            for (int g = 0; g < column.glyphs.size(); ++g) {
                const BotCockpitGlyph& glyph = column.glyphs.at(g);
                const double span = track_height;
                const double raw = std::fmod((offset * span) + travel +
                                                 (g * span / qMax(1, column.glyphs.size())),
                                             span);
                const double y = track_top + (raw < 0 ? raw + span : raw);
                // The head of the trail is bright, the tail fades — the fade is
                // a function of position only, never of a value.
                const int alpha = column.frozen ? 150
                                                : 90 + static_cast<int>(140.0 * (1.0 - (raw / span)));
                QColor ink = !glyph.known           ? QColor(colors::TEXT_SECONDARY())
                             : column.frozen        ? QColor(colors::WARNING())
                             : column.ignitions > 0 ? ignition
                             : dormant              ? QColor(colors::TEXT_SECONDARY())
                                                    : mood;
                painter.setPen(with_alpha(ink, qBound(40, alpha, 255)));
                painter.setFont(small_font);
                painter.drawText(QRectF(cell.left(), y, cell.width(), 12),
                                 Qt::AlignHCenter | Qt::AlignVCenter,
                                 QStringLiteral("%1 %2").arg(glyph.label, glyph.text));
            }

            if (column.settled) {
                painter.setPen(column.settled_won ? QColor(colors::GREEN()) : QColor(colors::RED()));
                painter.setFont(small_font);
                painter.drawText(
                    QRectF(cell.left(), cell.bottom() - 16, cell.width(), 14),
                    Qt::AlignHCenter | Qt::AlignVCenter,
                    QStringLiteral("%1 $%2")
                        .arg(column.settled_won ? QStringLiteral("WON") : QStringLiteral("LOST"))
                        .arg(column.settled_pnl_usd, 0, 'f', 2));
            }
        }

        // A window too narrow for every column says so. A silently truncated
        // rain would read as "these are all the contracts".
        if (drawn < scene_.columns.size()) {
            painter.setFont(small_font);
            painter.setPen(QColor(colors::WARNING()));
            painter.drawText(rain.adjusted(6, 0, -6, -2), Qt::AlignBottom | Qt::AlignRight,
                             QStringLiteral("%1 of %2 columns fit this window")
                                 .arg(drawn)
                                 .arg(scene_.columns.size()));
        }
    }

    // ── the decision envelope, over the rain ───────────────────────────────
    const int envelope_width = qMin(560, rain.width() - 40);
    const QRect envelope(rain.center().x() - (envelope_width / 2), rain.center().y() - 40,
                         envelope_width, 80);
    const QColor envelope_ink = role_color(scene_.envelope_role);
    painter.fillRect(envelope, with_alpha(QColor(colors::BG_BASE()), 232));
    painter.setPen(QPen(envelope_ink, 2));
    painter.drawRect(envelope);
    painter.setFont(small_font);
    painter.setPen(QColor(colors::TEXT_SECONDARY()));
    painter.drawText(envelope.adjusted(12, 6, -12, -54), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("DECISION ENVELOPE%1")
                         .arg(scene_.envelope_ticker.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" · %1").arg(scene_.envelope_ticker)));
    painter.setFont(title_font);
    painter.setPen(envelope_ink);
    painter.drawText(envelope.adjusted(12, 22, -12, -8), Qt::AlignLeft | Qt::AlignVCenter,
                     elide(QFontMetrics(title_font), scene_.envelope, envelope.width() - 24));

    // ── the ledger stream ──────────────────────────────────────────────────
    if (stream_width > 0) {
        painter.fillRect(stream, with_alpha(QColor(colors::BG_RAISED()), dormant ? 60 : 110));
        painter.setPen(QPen(with_alpha(mood, 70), 1));
        painter.drawRect(stream);
        painter.setFont(small_font);
        painter.setPen(QColor(colors::TEXT_SECONDARY()));
        painter.drawText(stream.adjusted(8, 4, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("LEDGER STREAM — one card per journaled event"));
        int y = stream.top() + 24;
        for (const auto& pulse : scene_.pulses) {
            if (y + 30 > stream.bottom()) break;
            const QRect card(stream.left() + 6, y, stream.width() - 12, 28);
            const QColor ink = role_color(pulse.role);
            // The card flashes only while its key is young: one flash per real
            // journal row, and none at all on a re-read of the same ledger.
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
            painter.drawText(stream.adjusted(8, 28, -8, 0), Qt::AlignTop | Qt::AlignLeft,
                             QStringLiteral("no journaled event in the ledger window"));
        }
    }

    // ── orbit nodes ────────────────────────────────────────────────────────
    if (!scene_.nodes.isEmpty()) {
        const double node_width =
            static_cast<double>(node_rect.width()) / scene_.nodes.size();
        for (int i = 0; i < scene_.nodes.size(); ++i) {
            const BotCockpitNode& node = scene_.nodes.at(i);
            const QRectF box(node_rect.left() + (i * node_width) + 3, node_rect.top(),
                             node_width - 6, node_rect.height());
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
    int kpi_x = kpi_rect.left() + 10;
    for (int i = 0; i < scene_.kpi.size(); ++i) {
        const QString entry = scene_.kpi.at(i);
        const QString role = i < scene_.kpi_roles.size() ? scene_.kpi_roles.at(i) : QString();
        const int entry_width = body_metrics.horizontalAdvance(entry);
        if (kpi_x + entry_width > kpi_rect.right() - 8) {
            // The strip ran out of room. Say so rather than eliding silently.
            painter.setPen(QColor(colors::WARNING()));
            painter.drawText(QRect(kpi_x, kpi_rect.top(), kpi_rect.right() - kpi_x - 4,
                                   kpi_rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             QStringLiteral("… %1 more").arg(scene_.kpi.size() - i));
            break;
        }
        painter.setPen(role.isEmpty() || role == QStringLiteral("grey") ? kpi_default
                                                                       : role_color(role));
        painter.drawText(QRect(kpi_x, kpi_rect.top(), entry_width, kpi_rect.height()),
                         Qt::AlignVCenter | Qt::AlignLeft, entry);
        kpi_x += entry_width;
        if (i + 1 < scene_.kpi.size()) {
            painter.setPen(QColor(colors::TEXT_SECONDARY()));
            const int separator_width = body_metrics.horizontalAdvance(kpi_separator);
            painter.drawText(QRect(kpi_x, kpi_rect.top(), separator_width, kpi_rect.height()),
                             Qt::AlignVCenter | Qt::AlignLeft, kpi_separator);
            kpi_x += separator_width;
        }
    }

}

} // namespace openmarketterminal::screens::kalshi
