#include "screens/crypto_trading/CryptoAutomationCockpit.h"

#include "core/config/ProfileManager.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace openmarketterminal::screens::crypto {

namespace {

QLabel* label(const QString& object_name, const QString& text = {}) {
    auto* out = new QLabel(text);
    out->setObjectName(object_name);
    return out;
}

QWidget* metric_card(QLabel*& value, QLabel*& caption) {
    auto* card = new QFrame;
    card->setObjectName("cryptoCockpitMetric");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(2);
    value = label("cryptoCockpitMetricValue", "--");
    caption = label("cryptoCockpitMetricLabel", "--");
    layout->addWidget(value);
    layout->addWidget(caption);
    return card;
}

QString daemon_file(const QString& name) {
    return openmarketterminal::ProfileManager::instance().profile_root()
           + QStringLiteral("/daemon/") + name;
}

QString tone_style(const QString& tone) {
    using namespace openmarketterminal::ui;
    if (tone == QLatin1String("good")) return colors::POSITIVE();
    if (tone == QLatin1String("bad")) return colors::NEGATIVE();
    if (tone == QLatin1String("warn")) return colors::WARNING();
    if (tone == QLatin1String("info")) return colors::CYAN();
    return colors::TEXT_PRIMARY();
}

} // namespace

CryptoAutomationCockpit::CryptoAutomationCockpit(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoAutomationCockpit");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* title = label("cryptoCockpitTitle", tr("COINBASE SPOT / SCALP COCKPIT"));
    header->addWidget(title);
    header->addStretch();
    heartbeat_value_ = label("cryptoCockpitHeartbeat", tr("WAITING FOR DAEMON"));
    header->addWidget(heartbeat_value_);
    refresh_btn_ = new QPushButton(tr("REFRESH"));
    refresh_btn_->setObjectName("cryptoCockpitRefresh");
    refresh_btn_->setToolTip(tr("Refresh local daemon state"));
    header->addWidget(refresh_btn_);
    layout->addLayout(header);

    auto* health_grid = new QGridLayout;
    health_grid->setContentsMargins(0, 0, 0, 0);
    health_grid->setHorizontalSpacing(8);
    health_grid->setVerticalSpacing(0);
    health_grid->addWidget(metric_card(engine_value_, engine_label_), 0, 0);
    health_grid->addWidget(metric_card(guard_value_, guard_label_), 0, 1);
    health_grid->addWidget(metric_card(cadence_value_, cadence_label_), 0, 2);
    health_grid->addWidget(metric_card(fee_value_, fee_label_), 0, 3);
    for (int col = 0; col < 4; ++col) health_grid->setColumnStretch(col, 1);
    layout->addLayout(health_grid);

    auto* decision_frame = new QFrame;
    decision_frame->setObjectName("cryptoCockpitDecision");
    auto* decision_grid = new QGridLayout(decision_frame);
    decision_grid->setContentsMargins(10, 8, 10, 8);
    decision_grid->setHorizontalSpacing(18);
    decision_grid->setVerticalSpacing(4);

    auto* decision_title = label("cryptoCockpitSection", tr("LATEST DECISION"));
    decision_grid->addWidget(decision_title, 0, 0, 1, 6);
    const auto add_field = [&decision_grid](int column, const QString& name, QLabel*& value) {
        auto* title = label("cryptoCockpitField", name);
        value = label("cryptoCockpitFieldValue", "--");
        decision_grid->addWidget(title, 1, column);
        decision_grid->addWidget(value, 2, column);
        decision_grid->setColumnStretch(column, 1);
    };
    add_field(0, tr("VERDICT"), verdict_value_);
    add_field(1, tr("DIRECTION"), direction_value_);
    add_field(2, tr("REFERENCE"), price_value_);
    add_field(3, tr("REQUIRED"), hurdle_value_);
    add_field(4, tr("NET AFTER COST"), edge_value_);
    decision_grid->addWidget(label("cryptoCockpitField", tr("BLOCKERS")), 3, 0, 1, 6);
    blockers_value_ = label("cryptoCockpitBlockers", "--");
    blockers_value_->setWordWrap(true);
    decision_grid->addWidget(blockers_value_, 4, 0, 1, 6);
    layout->addWidget(decision_frame);

    auto* sources_frame = new QFrame;
    sources_frame->setObjectName("cryptoCockpitSources");
    auto* sources_layout = new QVBoxLayout(sources_frame);
    sources_layout->setContentsMargins(10, 7, 10, 7);
    sources_layout->setSpacing(3);
    sources_layout->addWidget(label("cryptoCockpitSection", tr("MARKET FEEDS")));
    sources_value_ = label("cryptoCockpitSourcesValue", "--");
    sources_value_->setWordWrap(true);
    sources_layout->addWidget(sources_value_);
    layout->addWidget(sources_frame);

    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* positions_btn = new QPushButton(tr("OPEN POSITIONS"));
    positions_btn->setObjectName("cryptoCockpitAction");
    auto* orders_btn = new QPushButton(tr("OPEN ORDERS"));
    orders_btn->setObjectName("cryptoCockpitAction");
    actions->addWidget(positions_btn);
    actions->addWidget(orders_btn);
    layout->addLayout(actions);

    setStyleSheet(QString(R"(
        #cryptoAutomationCockpit { background:%1; }
        #cryptoCockpitTitle { color:%2; font-family:%3; font-weight:700; font-size:15px; }
        #cryptoCockpitHeartbeat { color:%4; font-family:%3; font-size:11px; }
        #cryptoCockpitRefresh, #cryptoCockpitAction { min-height:24px; padding:2px 10px; color:%5; background:%1; border:1px solid %6; font-family:%3; font-weight:700; }
        #cryptoCockpitRefresh:hover, #cryptoCockpitAction:hover { background:%7; }
        #cryptoCockpitMetric, #cryptoCockpitDecision, #cryptoCockpitSources { border:1px solid %8; background:%9; }
        #cryptoCockpitMetricValue { font-family:%3; font-size:19px; font-weight:700; }
        #cryptoCockpitMetricLabel, #cryptoCockpitField, #cryptoCockpitSection { color:%10; font-family:%3; font-size:10px; font-weight:700; }
        #cryptoCockpitFieldValue { color:%11; font-family:%3; font-size:14px; font-weight:700; }
        #cryptoCockpitBlockers, #cryptoCockpitSourcesValue { color:%10; font-family:%3; font-size:11px; }
    )")
        .arg(openmarketterminal::ui::colors::BG_BASE(), openmarketterminal::ui::colors::ORANGE(),
             openmarketterminal::ui::fonts::DATA_FAMILY(), openmarketterminal::ui::colors::TEXT_SECONDARY(),
             openmarketterminal::ui::colors::TEXT_PRIMARY(), openmarketterminal::ui::colors::BORDER_MED(),
             openmarketterminal::ui::colors::BG_HOVER(), openmarketterminal::ui::colors::BORDER_DIM(),
             openmarketterminal::ui::colors::BG_SURFACE(), openmarketterminal::ui::colors::TEXT_TERTIARY(),
             openmarketterminal::ui::colors::TEXT_PRIMARY()));

    connect(refresh_btn_, &QPushButton::clicked, this, &CryptoAutomationCockpit::refresh);
    connect(positions_btn, &QPushButton::clicked, this, &CryptoAutomationCockpit::positions_requested);
    connect(orders_btn, &QPushButton::clicked, this, &CryptoAutomationCockpit::orders_requested);

    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(1000);
    connect(refresh_timer_, &QTimer::timeout, this, &CryptoAutomationCockpit::refresh);
    refresh_timer_->start();
    refresh();
}

QJsonObject CryptoAutomationCockpit::read_json(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

QString CryptoAutomationCockpit::age_text(const QString& iso_time) {
    const QDateTime at = QDateTime::fromString(iso_time, Qt::ISODateWithMs);
    if (!at.isValid()) return QObject::tr("NO HEARTBEAT");
    const qint64 seconds = at.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0) return QObject::tr("LIVE NOW");
    if (seconds < 60) return QObject::tr("LIVE %1s AGO").arg(seconds);
    return QObject::tr("STALE %1m AGO").arg(seconds / 60);
}

QString CryptoAutomationCockpit::money_bps(double bps) {
    return QString::number(bps, 'f', 1) + QStringLiteral(" bps");
}

void CryptoAutomationCockpit::set_metric(QLabel* value, QLabel* label, const QString& text, const QString& tone) {
    value->setText(text);
    value->setStyleSheet(QStringLiteral("color:%1;").arg(tone_style(tone)));
    if (label && label->text().isEmpty()) label->setText(QObject::tr("STATUS"));
}

void CryptoAutomationCockpit::render_sources(const QJsonObject& decision) {
    const QJsonArray sources = decision.value(QStringLiteral("microstructure")).toObject()
                                   .value(QStringLiteral("sources")).toArray();
    QStringList parts;
    for (const QJsonValue& source_value : sources) {
        const QJsonObject source = source_value.toObject();
        const QString name = source.value(QStringLiteral("source")).toString().toUpper();
        const QString state = source.value(QStringLiteral("status")).toString().toUpper();
        const QJsonValue age_value = source.value(QStringLiteral("age_ms"));
        const qint64 age_ms = age_value.isString()
                                  ? age_value.toString().toLongLong()
                                  : static_cast<qint64>(age_value.toDouble(-1));
        if (!name.isEmpty())
            parts << QStringLiteral("%1 %2 %3ms").arg(name, state, age_ms >= 0 ? QString::number(age_ms) : QStringLiteral("--"));
    }
    sources_value_->setText(parts.isEmpty() ? tr("No current cross-venue feed snapshot.") : parts.join(QStringLiteral("  |  ")));
}

void CryptoAutomationCockpit::refresh() {
    const QJsonObject config_file = read_json(daemon_file(QStringLiteral("scalp_engine.json")));
    const QJsonObject state = read_json(daemon_file(QStringLiteral("scalp_state.json")));
    const QJsonObject guard = read_json(daemon_file(QStringLiteral("automation_live_guard.json")));
    const QJsonObject config = state.value(QStringLiteral("config")).toObject(config_file);
    const QJsonArray decisions = state.value(QStringLiteral("decisions")).toArray();
    const QJsonObject decision = decisions.isEmpty() ? QJsonObject{} : decisions.first().toObject();

    const bool running = state.value(QStringLiteral("status")).toString() == QLatin1String("running") && config.value(QStringLiteral("enabled")).toBool();
    set_metric(engine_value_, engine_label_, running ? tr("RUNNING") : tr("OFFLINE"), running ? QStringLiteral("good") : QStringLiteral("bad"));
    engine_label_->setText(tr("SCALP ENGINE"));

    const QDateTime expires = QDateTime::fromString(guard.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    const bool live_armed = guard.value(QStringLiteral("enabled")).toBool() && (!expires.isValid() || expires >= QDateTime::currentDateTimeUtc());
    set_metric(guard_value_, guard_label_, live_armed ? tr("LIVE ARMED") : tr("PAPER ONLY"), live_armed ? QStringLiteral("warn") : QStringLiteral("info"));
    guard_label_->setText(tr("EXECUTION GUARD"));

    const int cadence = config.value(QStringLiteral("cadence_ms")).toInt();
    set_metric(cadence_value_, cadence_label_, cadence > 0 ? tr("%1 ms").arg(cadence) : QStringLiteral("--"), QStringLiteral("info"));
    cadence_label_->setText(tr("DECISION CADENCE"));

    const double required_bps = decision.value(QStringLiteral("required_edge_bps")).toDouble();
    set_metric(fee_value_, fee_label_, required_bps > 0 ? money_bps(required_bps) : QStringLiteral("--"), QStringLiteral("warn"));
    fee_label_->setText(tr("MOVE REQUIRED"));

    const QString verdict = decision.value(QStringLiteral("verdict")).toString(tr("WAITING"));
    verdict_value_->setText(verdict);
    verdict_value_->setStyleSheet(QStringLiteral("color:%1;").arg(tone_style(verdict == QLatin1String("PAPER TRADE CANDIDATE") ? QStringLiteral("good") : QStringLiteral("warn"))));
    direction_value_->setText(decision.value(QStringLiteral("direction")).toString(QStringLiteral("--")).toUpper());
    const double reference = decision.value(QStringLiteral("reference_price")).toDouble();
    price_value_->setText(reference > 0.0 ? QStringLiteral("$%1").arg(QString::number(reference, 'f', 2)) : QStringLiteral("--"));
    hurdle_value_->setText(required_bps > 0.0 ? money_bps(required_bps) : QStringLiteral("--"));
    const double net_bps = decision.value(QStringLiteral("net_after_cost_bps")).toDouble();
    edge_value_->setText(decision.contains(QStringLiteral("net_after_cost_bps")) ? money_bps(net_bps) : QStringLiteral("--"));
    edge_value_->setStyleSheet(QStringLiteral("color:%1;").arg(tone_style(net_bps > 0.0 ? QStringLiteral("good") : QStringLiteral("bad"))));

    QStringList blockers;
    for (const QJsonValue& blocker : decision.value(QStringLiteral("blockers")).toArray()) blockers << blocker.toString();
    blockers_value_->setText(blockers.isEmpty() ? tr("No current blockers.") : blockers.join(QStringLiteral("  |  ")));
    render_sources(decision);

    const QString heartbeat = state.value(QStringLiteral("heartbeat_at")).toString();
    heartbeat_value_->setText(age_text(heartbeat));
    heartbeat_value_->setStyleSheet(QStringLiteral("color:%1;").arg(tone_style(running ? QStringLiteral("good") : QStringLiteral("bad"))));
}

} // namespace openmarketterminal::screens::crypto
