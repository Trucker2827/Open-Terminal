#include "screens/algo_trading/StrategyOpsMapPanel.h"
#include "screens/algo_trading/StrategyCockpitNavigation.h"
#include "screens/algo_trading/StrategyEvidencePresentation.h"

#include "core/config/ProfileManager.h"
#include "services/sandbox/SandboxEligibility.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QVariantList>
#include <QtMath>
#include <algorithm>

namespace openmarketterminal::screens {

namespace {

QJsonObject parse_params(const QString& json) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    return err.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

int scalar_int(const QString& sql, const QVariantList& args = {}) {
    auto r = Database::instance().execute(sql, args);
    if (!r.is_ok() || !r.value().next())
        return 0;
    return r.value().value(0).toInt();
}

QString money(double value) {
    const QString sign = value > 0.0 ? QStringLiteral("+") : QString();
    return QStringLiteral("%1$%2").arg(sign, QString::number(value, 'f', 2));
}

QString short_kind(QString kind) {
    kind.replace(QStringLiteral("chronos2_"), QStringLiteral("c2 "));
    kind.replace(QStringLiteral("long_short"), QStringLiteral("long/short"));
    return kind;
}

} // namespace

StrategyOpsMapPanel::StrategyOpsMapPanel(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(520);
    setObjectName(QStringLiteral("strategyOpsMapPanel"));
    setAutoFillBackground(false);
    setMouseTracking(true);

    frame_timer_.setInterval(33);
    connect(&frame_timer_, &QTimer::timeout, this, &StrategyOpsMapPanel::animate);

    refresh_timer_.setInterval(5000);
    connect(&refresh_timer_, &QTimer::timeout, this, &StrategyOpsMapPanel::refresh);
}

void StrategyOpsMapPanel::mouseMoveEvent(QMouseEvent* event) {
    hover_position_ = event->position();
    const auto hit = strategy_cockpit_hit(hover_position_, size(), phase_, visible_book_count());
    if (hit.view == StrategyCockpitView::None) {
        unsetCursor();
        setToolTip({});
    } else {
        setCursor(Qt::PointingHandCursor);
        const int book_index = hit.book_index < 0 ? -1 : visible_book_start() + hit.book_index;
        if (book_index >= 0 && book_index < books_.size()) {
            const auto& b = books_.at(book_index);
            const QString age = b.data_age_ms < 0 ? QStringLiteral("—")
                : tr("%1s").arg(b.data_age_ms / 1000);
            setToolTip(tr("%1\nMarket: %2\nHorizon: %3\nAuthority: %4\nProducer: %5 (%6)\nData age: %7\nLedger: %8\nLast error: %9\nLast decision: %10\nSkip totals: %11")
                .arg(b.strategy_id, b.market, b.horizon, b.authority, b.source,
                     b.producer_status, age, b.ledger,
                     b.last_error.isEmpty() ? QStringLiteral("—") : b.last_error,
                     b.last_decision.isEmpty() ? QStringLiteral("—") : b.last_decision,
                     b.skip_reasons.isEmpty() ? QStringLiteral("—") : b.skip_reasons));
        } else {
            setToolTip(tr(hit.label));
        }
    }
    update();
    QWidget::mouseMoveEvent(event);
}

void StrategyOpsMapPanel::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const auto hit = strategy_cockpit_hit(event->position(), size(), phase_, visible_book_count());
        if (hit.view != StrategyCockpitView::None) {
            const int book_index = hit.book_index < 0 ? -1 : visible_book_start() + hit.book_index;
            const QString kind = book_index >= 0 && book_index < books_.size()
                ? books_.at(book_index).kind : QString();
            emit drilldownRequested(static_cast<int>(hit.view), kind);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void StrategyOpsMapPanel::leaveEvent(QEvent* event) {
    hover_position_ = {-1.0, -1.0};
    unsetCursor();
    setToolTip({});
    update();
    QWidget::leaveEvent(event);
}

void StrategyOpsMapPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
    if (!frame_timer_.isActive())
        frame_timer_.start();
    if (!refresh_timer_.isActive())
        refresh_timer_.start();
}

void StrategyOpsMapPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    frame_timer_.stop();
    refresh_timer_.stop();
}

void StrategyOpsMapPanel::animate() {
    phase_ += 0.012;
    if (phase_ > 1000.0)
        phase_ = 0.0;
    update();
}

void StrategyOpsMapPanel::refresh() {
    books_.clear();
    active_books_ = chronos_books_ = spot_books_ = hypothetical_books_ = 0;
    open_positions_ = resolved_total_ = eligible_books_ = no_edge_books_ = 0;
    net_pnl_total_ = 0.0;
    decision_envelopes_ = trade_candidates_ = 0;
    latest_decision_verdict_ = QStringLiteral("WAITING");
    latest_decision_blocker_.clear();

    decision_envelopes_ = scalar_int(QStringLiteral("SELECT COUNT(*) FROM decision_envelopes"));
    trade_candidates_ = scalar_int(
        QStringLiteral("SELECT COUNT(*) FROM decision_envelopes WHERE verdict='TRADE_CANDIDATE'"));
    auto latest_envelope = Database::instance().execute(
        QStringLiteral("SELECT verdict,envelope_json FROM decision_envelopes ORDER BY decision_ts DESC LIMIT 1"));
    if (latest_envelope.is_ok() && latest_envelope.value().next()) {
        latest_decision_verdict_ = latest_envelope.value().value(0).toString();
        const QJsonObject envelope = parse_params(latest_envelope.value().value(1).toString());
        const QJsonArray blockers = envelope.value(QStringLiteral("risk_blockers")).toArray();
        if (!blockers.isEmpty()) latest_decision_blocker_ = blockers.first().toString();
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString profile = ProfileManager::instance().active();
    auto registry = services::sandbox::strategy_registry_snapshot(profile, now_ms);
    if (registry.is_err()) {
        status_text_ = QString::fromStdString(registry.error());
        last_refresh_ms_ = QDateTime::currentMSecsSinceEpoch();
        update();
        return;
    }
    const QJsonObject registry_object = registry.value();
    const QJsonObject registry_summary = registry_object.value(QStringLiteral("summary")).toObject();
    for (const QJsonValue& value : registry_object.value(QStringLiteral("strategies")).toArray()) {
        const QJsonObject row = value.toObject();
        BookNode b;
        b.strategy_id = row.value(QStringLiteral("strategy_id")).toString();
        b.kind = row.value(QStringLiteral("kind")).toString();
        b.source = row.value(QStringLiteral("producer")).toString();
        b.market = row.value(QStringLiteral("market")).toString();
        b.horizon = row.value(QStringLiteral("horizon")).toString();
        b.authority = row.value(QStringLiteral("authority")).toString();
        b.ledger = row.value(QStringLiteral("ledger")).toString();
        b.status = row.value(QStringLiteral("book_status")).toString();
        b.producer_status = row.value(QStringLiteral("producer_status")).toString();
        b.data_age_ms = row.value(QStringLiteral("data_age_ms")).toVariant().toLongLong();
        b.last_error = row.value(QStringLiteral("last_error")).toString();
        b.skip_reasons = QString::fromUtf8(QJsonDocument(
            row.value(QStringLiteral("skip_reasons")).toObject()).toJson(QJsonDocument::Compact));
        b.last_decision = QString::fromUtf8(QJsonDocument(
            row.value(QStringLiteral("last_decision")).toObject()).toJson(QJsonDocument::Compact));
        b.hypothetical = row.value(QStringLiteral("hypothetical")).toBool();
        b.price_forecast = b.kind.contains(QStringLiteral("chronos2"));
        b.chronos = b.kind.startsWith(QStringLiteral("chronos2"));
        b.resolved = row.value(QStringLiteral("resolved")).toInt();
        b.net_pnl = row.value(QStringLiteral("net_pnl")).toDouble();
        b.hit_rate = row.value(QStringLiteral("hit_rate")).toDouble();
        b.open = row.value(QStringLiteral("open")).toInt();
        b.eligible = row.value(QStringLiteral("eligible")).toBool();
        b.no_edge = strategy_proof_state(b.hypothetical, b.eligible, b.resolved, b.net_pnl,
                                         services::sandbox::kMinResolvedSample) == StrategyProofState::NoEdge;
        net_pnl_total_ += b.net_pnl;
        resolved_total_ += b.resolved;
        open_positions_ += b.open;
        if (b.eligible)
            ++eligible_books_;
        if (b.no_edge)
            ++no_edge_books_;

        if (b.status == QStringLiteral("active"))
            ++active_books_;
        if (b.chronos)
            ++chronos_books_;
        if (b.kind == QStringLiteral("spot"))
            ++spot_books_;
        if (b.hypothetical)
            ++hypothetical_books_;

        books_.append(b);
    }

    std::sort(books_.begin(), books_.end(), [](const BookNode& a, const BookNode& b) {
        if (a.eligible != b.eligible)
            return a.eligible;
        if (a.open != b.open)
            return a.open > b.open;
        if (a.resolved != b.resolved)
            return a.resolved > b.resolved;
        return a.kind < b.kind;
    });

    last_refresh_ms_ = QDateTime::currentMSecsSinceEpoch();
    status_text_ = books_.isEmpty()
        ? tr("No evidence books yet. Seed books from Evidence to start the machine.")
        : tr("Registry: %1 stale · %2 errors · %3 waiting. Latest decision: %4%5")
              .arg(registry_summary.value(QStringLiteral("stale")).toInt())
              .arg(registry_summary.value(QStringLiteral("errors")).toInt())
              .arg(registry_summary.value(QStringLiteral("waiting")).toInt())
              .arg(latest_decision_verdict_, latest_decision_blocker_.isEmpty()
                    ? QString() : QStringLiteral(" — ") + latest_decision_blocker_);
    update();
}

void StrategyOpsMapPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect().adjusted(0, 0, -1, -1);
    draw_background(p, r);
    draw_hud(p, r);
    draw_flow(p, r);
    draw_book_orbit(p, r);
}

void StrategyOpsMapPanel::draw_background(QPainter& p, const QRectF& r) {
    p.fillRect(r, QColor(ui::colors::BG_BASE()));

    QRadialGradient glow(r.center(), qMax(r.width(), r.height()) * 0.55);
    glow.setColorAt(0.0, QColor(20, 184, 166, 34));
    glow.setColorAt(0.45, QColor(217, 119, 6, 18));
    glow.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(r, glow);

    p.setPen(QPen(QColor(ui::colors::BORDER_DIM()), 1));
    const qreal grid = 48.0;
    for (qreal x = std::fmod(phase_ * 18.0, grid) - grid; x < r.width(); x += grid)
        p.drawLine(QPointF(x, r.top()), QPointF(x + r.height() * 0.18, r.bottom()));
    for (qreal y = 70.0; y < r.height(); y += grid)
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
}

void StrategyOpsMapPanel::draw_hud(QPainter& p, const QRectF& r) {
    p.save();
    p.setFont(QFont(ui::fonts::DATA_FAMILY(), 10, QFont::Bold));
    p.setPen(QColor(ui::colors::AMBER()));
    p.drawText(QRectF(18, 16, 420, 24), Qt::AlignLeft | Qt::AlignVCenter, tr("LOCAL STRATEGY CONTROL CENTER"));

    p.setFont(QFont(ui::fonts::DATA_FAMILY(), 8));
    p.setPen(QColor(ui::colors::TEXT_SECONDARY()));
    const QString age = last_refresh_ms_ > 0
        ? tr("refreshed %1s ago").arg((QDateTime::currentMSecsSinceEpoch() - last_refresh_ms_) / 1000)
        : tr("waiting for first refresh");
    const int pages = qMax(1, (books_.size() + 13) / 14);
    const int page = books_.isEmpty() ? 0 : visible_book_start() / 14;
    const QString page_status = pages > 1 ? tr("  |  registry page %1/%2 · rotates every 15s").arg(page + 1).arg(pages) : QString();
    p.drawText(QRectF(18, 40, r.width() - 36, 22), Qt::AlignLeft | Qt::AlignVCenter,
               status_text_ + QStringLiteral("  |  ") + age + page_status);

    struct Chip { QString label; QString value; QColor color; };
    const QVector<Chip> chips = {
        {tr("BOOKS"), QString::number(books_.size()), QColor(ui::colors::TEXT_PRIMARY())},
        {tr("ENVELOPES"), QString::number(decision_envelopes_), QColor(ui::colors::CYAN())},
        {tr("CHRONOS"), QString::number(chronos_books_), QColor(167, 139, 250)},
        {tr("OPEN"), QString::number(open_positions_), QColor(ui::colors::AMBER())},
        {tr("RESOLVED"), QString::number(resolved_total_), QColor(ui::colors::CYAN())},
        {tr("ELIGIBLE"), QString::number(eligible_books_), QColor(ui::colors::POSITIVE())},
        {tr("NO EDGE"), QString::number(no_edge_books_), no_edge_books_ > 0 ? QColor(ui::colors::NEGATIVE()) : QColor(ui::colors::TEXT_PRIMARY())},
        {tr("NET"), money(net_pnl_total_), net_pnl_total_ >= 0.0 ? QColor(ui::colors::POSITIVE()) : QColor(ui::colors::NEGATIVE())},
    };
    qreal x = 18;
    for (const auto& chip : chips) {
        QRectF box(x, 72, 126, 42);
        const bool hovered = box.contains(hover_position_);
        p.setPen(QPen(hovered ? chip.color : QColor(ui::colors::BORDER_DIM()), hovered ? 2 : 1));
        p.setBrush(QColor(255, 255, 255, hovered ? 18 : 8));
        p.drawRoundedRect(box, 2, 2);
        p.setPen(QColor(ui::colors::TEXT_TERTIARY()));
        p.setFont(QFont(ui::fonts::DATA_FAMILY(), 7, QFont::Bold));
        p.drawText(box.adjusted(8, 4, -8, -22), Qt::AlignLeft | Qt::AlignVCenter, chip.label);
        p.setPen(chip.color);
        p.setFont(QFont(ui::fonts::DATA_FAMILY(), 13, QFont::Bold));
        p.drawText(box.adjusted(8, 17, -8, -3), Qt::AlignLeft | Qt::AlignVCenter, chip.value);
        x += box.width() + 8;
    }
    p.restore();
}

void StrategyOpsMapPanel::draw_flow(QPainter& p, const QRectF& r) {
    const QPointF center(r.center().x(), r.center().y() + 42);
    const qreal w = r.width();
    const qreal h = r.height();

    const QVector<QPair<QString, QPointF>> left_nodes = {
        {tr("COINBASE TICKS"), QPointF(w * 0.18, h * 0.34)},
        {tr("KALSHI/ODDS"), QPointF(w * 0.15, h * 0.52)},
        {tr("NEWS + NOTES"), QPointF(w * 0.20, h * 0.70)},
    };
    const QVector<QPair<QString, QPointF>> right_nodes = {
        {tr("RISK GATE"), QPointF(w * 0.82, h * 0.34)},
        {tr("PAPER EXECUTOR"), QPointF(w * 0.86, h * 0.52)},
        {tr("OUTCOME + SCORE"), QPointF(w * 0.80, h * 0.70)},
    };

    for (int i = 0; i < left_nodes.size(); ++i) {
        const QColor c = i == 0 ? QColor(ui::colors::CYAN()) : (i == 1 ? QColor(ui::colors::AMBER()) : QColor(167, 139, 250));
        draw_particle_line(p, left_nodes[i].second, center, c, i * 0.21);
        draw_node(p, left_nodes[i].second, 42, left_nodes[i].first, tr("observer"), c, true);
    }

    draw_node(p, QPointF(center.x(), center.y() - 116), 48, tr("CHRONOS-2"), tr("forecast books"), QColor(167, 139, 250), chronos_books_ > 0);
    draw_particle_line(p, QPointF(center.x(), center.y() - 116), center, QColor(167, 139, 250), 0.37);

    draw_node(p, center, 64, tr("DECISION ENVELOPE"), latest_decision_verdict_,
              QColor(ui::colors::AMBER()), decision_envelopes_ > 0);

    for (int i = 0; i < right_nodes.size(); ++i) {
        const QColor c = i == 0 ? QColor(ui::colors::POSITIVE()) : (i == 1 ? QColor(ui::colors::CYAN()) : QColor(ui::colors::AMBER()));
        draw_particle_line(p, center, right_nodes[i].second, c, 0.44 + i * 0.19);
        draw_node(p, right_nodes[i].second, 42, right_nodes[i].first,
                  i == 0 ? tr("%1 candidates").arg(trade_candidates_)
                         : (i == 1 ? tr("paper fills") : tr("settles + learns")), c, true);
    }
}

void StrategyOpsMapPanel::draw_book_orbit(QPainter& p, const QRectF& r) {
    if (books_.isEmpty())
        return;

    const QPointF center(r.center().x(), r.center().y() + 42);
    const qreal radius = qMin(r.width(), r.height()) * 0.31;
    p.save();
    p.setPen(QPen(QColor(20, 184, 166, 50), 1));
    for (int ring = 0; ring < 3; ++ring)
        p.drawEllipse(center, radius * (0.72 + ring * 0.14), radius * (0.72 + ring * 0.14));

    const int start = visible_book_start();
    const int n = visible_book_count();
    for (int i = 0; i < n; ++i) {
        const auto& b = books_.at(start + i);
        const qreal angle = -M_PI_2 + (2.0 * M_PI * i / qMax(1, n)) + phase_ * 0.08;
        const qreal pulse = 1.0 + 0.06 * std::sin(phase_ * 4.0 + i);
        const QPointF pos(center.x() + std::cos(angle) * radius,
                          center.y() + std::sin(angle) * radius * 0.68);
        const QColor c = color_for_book(b);
        draw_particle_line(p, center, pos, c, 0.08 * i);
        const QString sub = tr("%1 · %2 · %3").arg(
            b.market.isEmpty() ? QStringLiteral("—") : b.market,
            b.horizon.isEmpty() ? QStringLiteral("—") : b.horizon,
            b.producer_status.isEmpty() ? QStringLiteral("UNKNOWN") : b.producer_status);
        draw_node(p, pos, 33 * pulse,
                  b.strategy_id.isEmpty() ? short_kind(b.kind) : b.strategy_id,
                  sub, c, b.status == QStringLiteral("active"));
    }
    p.restore();
}

int StrategyOpsMapPanel::visible_book_start() const {
    constexpr int page_size = 14;
    if (books_.size() <= page_size)
        return 0;
    const int pages = (books_.size() + page_size - 1) / page_size;
    const qint64 page = (QDateTime::currentMSecsSinceEpoch() / 15000) % pages;
    return static_cast<int>(page) * page_size;
}

int StrategyOpsMapPanel::visible_book_count() const {
    return qMin(14, qMax(0, books_.size() - visible_book_start()));
}

void StrategyOpsMapPanel::draw_node(QPainter& p, const QPointF& c, qreal radius, const QString& title,
                                    const QString& subtitle, const QColor& color, bool active) {
    p.save();
    const qreal glow = radius * (1.25 + 0.08 * std::sin(phase_ * 5.0 + c.x() * 0.01));
    QRadialGradient g(c, glow);
    QColor glow_color = color;
    glow_color.setAlpha(active ? 72 : 28);
    g.setColorAt(0.0, glow_color);
    glow_color.setAlpha(0);
    g.setColorAt(1.0, glow_color);
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, glow, glow);

    QRectF box(c.x() - radius, c.y() - radius * 0.58, radius * 2.0, radius * 1.16);
    const bool hovered = box.contains(hover_position_);
    QLinearGradient lg(box.topLeft(), box.bottomRight());
    lg.setColorAt(0.0, QColor(255, 255, 255, active ? 22 : 10));
    lg.setColorAt(1.0, QColor(0, 0, 0, 40));
    p.setBrush(lg);
    p.setPen(QPen(color, hovered ? 2.8 : (active ? 1.8 : 1.0)));
    p.drawRoundedRect(box, 5, 5);

    p.setFont(QFont(ui::fonts::DATA_FAMILY(), qMax(7, static_cast<int>(radius / 5.0)), QFont::Bold));
    p.setPen(QColor(ui::colors::TEXT_PRIMARY()));
    p.drawText(box.adjusted(5, 2, -5, -box.height() * 0.44), Qt::AlignCenter, title);
    p.setFont(QFont(ui::fonts::DATA_FAMILY(), qMax(6, static_cast<int>(radius / 6.5))));
    p.setPen(QColor(ui::colors::TEXT_TERTIARY()));
    p.drawText(box.adjusted(5, box.height() * 0.48, -5, -2), Qt::AlignCenter, subtitle);
    p.restore();
}

void StrategyOpsMapPanel::draw_particle_line(QPainter& p, const QPointF& a, const QPointF& b, const QColor& color,
                                             qreal phase_offset) {
    p.save();
    QColor line = color;
    line.setAlpha(72);
    p.setPen(QPen(line, 1.4));
    QPainterPath path(a);
    const QPointF mid((a.x() + b.x()) / 2.0, (a.y() + b.y()) / 2.0 - 28.0 * std::sin((a.x() - b.x()) * 0.01));
    path.quadTo(mid, b);
    p.drawPath(path);

    const qreal t = std::fmod(phase_ * 0.55 + phase_offset, 1.0);
    const QPointF particle = path.pointAtPercent(t);
    QColor dot = color;
    dot.setAlpha(210);
    p.setBrush(dot);
    p.setPen(Qt::NoPen);
    p.drawEllipse(particle, 3.6, 3.6);
    p.restore();
}

QColor StrategyOpsMapPanel::color_for_book(const BookNode& b) const {
    if (b.producer_status == QLatin1String("ERROR"))
        return QColor(ui::colors::NEGATIVE());
    if (b.producer_status == QLatin1String("STALE") ||
        b.producer_status == QLatin1String("WARMING"))
        return QColor(ui::colors::AMBER());
    if (b.producer_status == QLatin1String("PAUSED") ||
        b.producer_status == QLatin1String("RETIRED") ||
        b.producer_status == QLatin1String("UNAVAILABLE") ||
        b.producer_status == QLatin1String("RESEARCH ONLY") ||
        b.producer_status == QLatin1String("ON DEMAND"))
        return QColor(ui::colors::TEXT_TERTIARY());
    if (b.eligible)
        return QColor(ui::colors::POSITIVE());
    if (b.no_edge)
        return QColor(ui::colors::NEGATIVE());
    if (b.hypothetical)
        return QColor(ui::colors::TEXT_TERTIARY());
    if (b.chronos)
        return QColor(167, 139, 250);
    if (b.kind == QStringLiteral("spot"))
        return QColor(ui::colors::CYAN());
    if (b.net_pnl < 0.0)
        return QColor(ui::colors::NEGATIVE());
    return QColor(ui::colors::AMBER());
}

} // namespace openmarketterminal::screens
