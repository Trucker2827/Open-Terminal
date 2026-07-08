#include "screens/algo_trading/StrategyOpsMapPanel.h"

#include "core/config/ProfileManager.h"
#include "services/sandbox/SandboxEligibility.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
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

qint64 scalar_i64(const QString& sql, const QVariantList& args = {}) {
    auto r = Database::instance().execute(sql, args);
    if (!r.is_ok() || !r.value().next() || r.value().value(0).isNull())
        return 0;
    return r.value().value(0).toLongLong();
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

    frame_timer_.setInterval(33);
    connect(&frame_timer_, &QTimer::timeout, this, &StrategyOpsMapPanel::animate);

    refresh_timer_.setInterval(5000);
    connect(&refresh_timer_, &QTimer::timeout, this, &StrategyOpsMapPanel::refresh);
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
    open_positions_ = resolved_total_ = eligible_books_ = 0;
    net_pnl_total_ = 0.0;

    auto strategies = services::sandbox::list_strategies();
    if (strategies.is_err()) {
        status_text_ = QString::fromStdString(strategies.error());
        last_refresh_ms_ = QDateTime::currentMSecsSinceEpoch();
        update();
        return;
    }

    const QString profile = ProfileManager::instance().active();
    auto board = services::sandbox::leaderboard(profile);
    QHash<QString, services::sandbox::LeaderboardRow> board_by_id;
    if (board.is_ok()) {
        for (const auto& row : board.value())
            board_by_id.insert(row.strategy_id, row);
    }

    for (const auto& row : strategies.value()) {
        if (row.status == QStringLiteral("retired"))
            continue;

        const QJsonObject params = parse_params(row.params_json);
        BookNode b;
        b.kind = row.kind;
        b.source = params.value(QStringLiteral("journal_source")).toString(
            params.value(QStringLiteral("source")).toString(QStringLiteral("edge journal")));
        b.horizon = params.value(QStringLiteral("horizon")).toString();
        if (b.horizon.isEmpty() && params.contains(QStringLiteral("horizon_sec")))
            b.horizon = QStringLiteral("%1s").arg(params.value(QStringLiteral("horizon_sec")).toInt());
        b.status = row.status;
        b.hypothetical = params.value(QStringLiteral("hypothetical")).toBool(false);
        b.price_forecast = params.value(QStringLiteral("price_forecast")).toBool(false);
        b.chronos = row.kind.startsWith(QStringLiteral("chronos2"));

        const auto lb = board_by_id.constFind(row.strategy_id);
        if (lb != board_by_id.constEnd()) {
            b.resolved = lb->resolved;
            b.net_pnl = lb->net_pnl;
            b.hit_rate = lb->hit_rate;
            net_pnl_total_ += lb->net_pnl;
            resolved_total_ += lb->resolved;
        }

        b.open = scalar_int(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND state IN ('open','pending_fill')"),
            {row.strategy_id});
        open_positions_ += b.open;

        const qint64 first_created = scalar_i64(
            QStringLiteral("SELECT MIN(created_at) FROM sandbox_position WHERE strategy_id = ?"), {row.strategy_id});
        const int total_positions = scalar_int(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ?"), {row.strategy_id});
        const int active_days = first_created > 0
            ? qMax(0, static_cast<int>((QDateTime::currentMSecsSinceEpoch() - first_created) / (24LL * 3600 * 1000)))
            : 0;

        services::sandbox::EligibilityInput in;
        in.active_days = active_days;
        in.resolved = b.resolved;
        in.total_positions = total_positions;
        in.net_pnl = b.net_pnl;
        in.hypothetical = b.hypothetical;
        if (lb != board_by_id.constEnd()) {
            in.degraded = lb->degraded;
            in.max_drawdown = lb->max_drawdown;
            in.gross_notional = lb->gross_notional;
        }
        b.eligible = services::sandbox::evaluate_eligibility(in).eligible;
        if (b.eligible)
            ++eligible_books_;

        if (row.status == QStringLiteral("active"))
            ++active_books_;
        if (b.chronos)
            ++chronos_books_;
        if (row.kind == QStringLiteral("spot"))
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
        ? tr("No proof books yet. Seed books from Proof Books to start the machine.")
        : tr("Sandbox/Chronos books are being watched locally. Paper-only evidence layer.");
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
    p.drawText(QRectF(18, 16, 360, 24), Qt::AlignLeft | Qt::AlignVCenter, tr("STRATEGY OPERATIONS MAP"));

    p.setFont(QFont(ui::fonts::DATA_FAMILY(), 8));
    p.setPen(QColor(ui::colors::TEXT_SECONDARY()));
    const QString age = last_refresh_ms_ > 0
        ? tr("refreshed %1s ago").arg((QDateTime::currentMSecsSinceEpoch() - last_refresh_ms_) / 1000)
        : tr("waiting for first refresh");
    p.drawText(QRectF(18, 40, r.width() - 36, 22), Qt::AlignLeft | Qt::AlignVCenter,
               status_text_ + QStringLiteral("  |  ") + age);

    struct Chip { QString label; QString value; QColor color; };
    const QVector<Chip> chips = {
        {tr("BOOKS"), QString::number(books_.size()), QColor(ui::colors::TEXT_PRIMARY())},
        {tr("ACTIVE"), QString::number(active_books_), QColor(ui::colors::POSITIVE())},
        {tr("CHRONOS"), QString::number(chronos_books_), QColor(167, 139, 250)},
        {tr("OPEN"), QString::number(open_positions_), QColor(ui::colors::AMBER())},
        {tr("RESOLVED"), QString::number(resolved_total_), QColor(ui::colors::CYAN())},
        {tr("ELIGIBLE"), QString::number(eligible_books_), QColor(ui::colors::POSITIVE())},
        {tr("NET"), money(net_pnl_total_), net_pnl_total_ >= 0.0 ? QColor(ui::colors::POSITIVE()) : QColor(ui::colors::NEGATIVE())},
    };
    qreal x = 18;
    for (const auto& chip : chips) {
        QRectF box(x, 72, 126, 42);
        p.setPen(QPen(QColor(ui::colors::BORDER_DIM()), 1));
        p.setBrush(QColor(255, 255, 255, 8));
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
        {tr("PAPER EXECUTOR"), QPointF(w * 0.82, h * 0.34)},
        {tr("OUTCOME RESOLVER"), QPointF(w * 0.86, h * 0.52)},
        {tr("LEADERBOARD"), QPointF(w * 0.80, h * 0.70)},
    };

    for (int i = 0; i < left_nodes.size(); ++i) {
        const QColor c = i == 0 ? QColor(ui::colors::CYAN()) : (i == 1 ? QColor(ui::colors::AMBER()) : QColor(167, 139, 250));
        draw_particle_line(p, left_nodes[i].second, center, c, i * 0.21);
        draw_node(p, left_nodes[i].second, 42, left_nodes[i].first, tr("observer"), c, true);
    }

    draw_node(p, QPointF(center.x(), center.y() - 116), 48, tr("CHRONOS-2"), tr("forecast books"), QColor(167, 139, 250), chronos_books_ > 0);
    draw_particle_line(p, QPointF(center.x(), center.y() - 116), center, QColor(167, 139, 250), 0.37);

    draw_node(p, center, 64, tr("RISK GATE"), tr("paper-only proof"), QColor(ui::colors::AMBER()), true);

    for (int i = 0; i < right_nodes.size(); ++i) {
        const QColor c = i == 0 ? QColor(ui::colors::POSITIVE()) : (i == 1 ? QColor(ui::colors::CYAN()) : QColor(ui::colors::AMBER()));
        draw_particle_line(p, center, right_nodes[i].second, c, 0.44 + i * 0.19);
        draw_node(p, right_nodes[i].second, 42, right_nodes[i].first, i == 0 ? tr("fills") : (i == 1 ? tr("settles") : tr("scores")), c, true);
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

    const int n = qMin(books_.size(), 14);
    for (int i = 0; i < n; ++i) {
        const auto& b = books_.at(i);
        const qreal angle = -M_PI_2 + (2.0 * M_PI * i / qMax(1, n)) + phase_ * 0.08;
        const qreal pulse = 1.0 + 0.06 * std::sin(phase_ * 4.0 + i);
        const QPointF pos(center.x() + std::cos(angle) * radius,
                          center.y() + std::sin(angle) * radius * 0.68);
        const QColor c = color_for_book(b);
        draw_particle_line(p, center, pos, c, 0.08 * i);
        const QString sub = b.horizon.isEmpty()
            ? tr("%1 resolved").arg(b.resolved)
            : tr("%1 | %2").arg(b.horizon, QString::number(b.resolved));
        draw_node(p, pos, 33 * pulse, short_kind(b.kind), sub, c, b.status == QStringLiteral("active"));
    }
    p.restore();
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
    QLinearGradient lg(box.topLeft(), box.bottomRight());
    lg.setColorAt(0.0, QColor(255, 255, 255, active ? 22 : 10));
    lg.setColorAt(1.0, QColor(0, 0, 0, 40));
    p.setBrush(lg);
    p.setPen(QPen(color, active ? 1.8 : 1.0));
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
    if (b.eligible)
        return QColor(ui::colors::POSITIVE());
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
