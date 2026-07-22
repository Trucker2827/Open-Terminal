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

namespace openmarketterminal::screens::crypto {

namespace {

using namespace openmarketterminal::ui;

QString daemon_file(const QString& name) {
    return openmarketterminal::ProfileManager::instance().profile_root() + QStringLiteral("/daemon/") + name;
}

QLabel* text_label(const QString& name, const QString& text = {}) {
    auto* out = new QLabel(text);
    out->setObjectName(name);
    return out;
}

QWidget* metric_card(QLabel*& value, QLabel*& caption) {
    auto* card = new QFrame;
    card->setObjectName("cryptoCockpitMetric");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(1);
    value = text_label("cryptoCockpitMetricValue", "--");
    caption = text_label("cryptoCockpitMetricCaption", "--");
    layout->addWidget(value);
    layout->addWidget(caption);
    return card;
}

QString venue_title(const QString& id) {
    if (id.compare(QLatin1String("coinbase"), Qt::CaseInsensitive) == 0)
        return QObject::tr("COINBASE ADVANCED");
    if (id.compare(QLatin1String("kraken"), Qt::CaseInsensitive) == 0)
        return QObject::tr("KRAKEN PRO");
    return id.toUpper();
}

} // namespace

CryptoAutomationCockpit::CryptoAutomationCockpit(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoAutomationCockpit");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(7);

    auto* header = new QHBoxLayout;
    header->addWidget(text_label("cryptoCockpitTitle", tr("SPOT / SCALP COCKPIT")));
    header->addStretch();
    heartbeat_value_ = text_label("cryptoCockpitHeartbeat", tr("WAITING FOR DAEMON"));
    header->addWidget(heartbeat_value_);
    auto* positions = new QPushButton(tr("POSITIONS"));
    auto* orders = new QPushButton(tr("ORDERS"));
    positions->setObjectName("cryptoCockpitAction");
    orders->setObjectName("cryptoCockpitAction");
    header->addWidget(positions);
    header->addWidget(orders);
    layout->addLayout(header);

    auto* cards = new QGridLayout;
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(7);
    cards->addWidget(metric_card(venue_value_, venue_caption_), 0, 0);
    cards->addWidget(metric_card(engine_value_, engine_caption_), 0, 1);
    cards->addWidget(metric_card(guard_value_, guard_caption_), 0, 2);
    cards->addWidget(metric_card(hurdle_value_, hurdle_caption_), 0, 3);
    for (int index = 0; index < 4; ++index) cards->setColumnStretch(index, 1);
    layout->addLayout(cards);

    auto* decision = new QFrame;
    decision->setObjectName("cryptoCockpitPanel");
    auto* grid = new QGridLayout(decision);
    grid->setContentsMargins(10, 7, 10, 7);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(3);
    grid->addWidget(text_label("cryptoCockpitSection", tr("LATEST SPOT / SCALP DECISION")), 0, 0, 1, 4);
    const auto field = [&grid](int column, const QString& name, QLabel*& value) {
        grid->addWidget(text_label("cryptoCockpitField", name), 1, column);
        value = text_label("cryptoCockpitFieldValue", "--");
        grid->addWidget(value, 2, column);
        grid->setColumnStretch(column, 1);
    };
    field(0, tr("VERDICT"), verdict_value_);
    field(1, tr("DIRECTION"), direction_value_);
    field(2, tr("REFERENCE"), price_value_);
    field(3, tr("NET AFTER COST"), edge_value_);
    grid->addWidget(text_label("cryptoCockpitField", tr("WHY THE ENGINE WAITED OR ACTED")), 3, 0, 1, 4);
    blockers_value_ = text_label("cryptoCockpitDetail", "--");
    blockers_value_->setWordWrap(true);
    grid->addWidget(blockers_value_, 4, 0, 1, 4);
    layout->addWidget(decision);

    auto* feeds = new QFrame;
    feeds->setObjectName("cryptoCockpitPanel");
    auto* feeds_layout = new QVBoxLayout(feeds);
    feeds_layout->setContentsMargins(10, 6, 10, 6);
    feeds_layout->setSpacing(2);
    feeds_layout->addWidget(text_label("cryptoCockpitSection", tr("CROSS-VENUE FEED HEALTH")));
    sources_value_ = text_label("cryptoCockpitDetail", "--");
    sources_value_->setWordWrap(true);
    feeds_layout->addWidget(sources_value_);
    layout->addWidget(feeds);

    setStyleSheet(QString(R"(
      #cryptoAutomationCockpit { background:%1; }
      #cryptoCockpitTitle { color:%2; font-family:%3; font-weight:700; font-size:15px; }
      #cryptoCockpitHeartbeat { color:%4; font-family:%3; font-size:11px; }
      #cryptoCockpitAction { min-height:23px; padding:2px 9px; color:%5; background:%1; border:1px solid %2; font-family:%3; font-weight:700; }
      #cryptoCockpitAction:hover { background:%6; }
      #cryptoCockpitMetric, #cryptoCockpitPanel { background:%7; border:1px solid %8; }
      #cryptoCockpitMetricValue { font-family:%3; font-size:17px; font-weight:700; }
      #cryptoCockpitMetricCaption, #cryptoCockpitField, #cryptoCockpitSection { color:%9; font-family:%3; font-size:10px; font-weight:700; }
      #cryptoCockpitFieldValue { color:%5; font-family:%3; font-size:13px; font-weight:700; }
      #cryptoCockpitDetail { color:%4; font-family:%3; font-size:11px; }
    )")
        .arg(colors::BG_BASE(), colors::ORANGE(), fonts::DATA_FAMILY(), colors::TEXT_SECONDARY(),
             colors::TEXT_PRIMARY(), colors::BG_HOVER(), colors::BG_SURFACE(), colors::BORDER_DIM(),
             colors::TEXT_TERTIARY()));

    connect(positions, &QPushButton::clicked, this, &CryptoAutomationCockpit::positions_requested);
    connect(orders, &QPushButton::clicked, this, &CryptoAutomationCockpit::orders_requested);
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(1000);
    connect(refresh_timer_, &QTimer::timeout, this, &CryptoAutomationCockpit::refresh);
    refresh_timer_->start();
    refresh();
}

void CryptoAutomationCockpit::set_exchange_context(const QString& exchange_id, bool is_paper) {
    exchange_id_ = exchange_id;
    is_paper_ = is_paper;
    refresh();
}

QJsonObject CryptoAutomationCockpit::read_json(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

QString CryptoAutomationCockpit::age_text(const QString& iso_time) {
    const auto at = QDateTime::fromString(iso_time, Qt::ISODateWithMs);
    if (!at.isValid()) return QObject::tr("NO DAEMON HEARTBEAT");
    const qint64 seconds = at.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0 || seconds < 60) return QObject::tr("DAEMON LIVE %1s").arg(qMax<qint64>(seconds, 0));
    return QObject::tr("DAEMON STALE %1m").arg(seconds / 60);
}

QString CryptoAutomationCockpit::bps_text(double bps) {
    return QString::number(bps, 'f', 1) + QStringLiteral(" bps");
}

void CryptoAutomationCockpit::set_metric(QLabel* value, QLabel* caption, const QString& text, const QString& color) {
    value->setText(text);
    value->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    Q_UNUSED(caption);
}

void CryptoAutomationCockpit::render_sources(const QJsonObject& decision) {
    const auto sources = decision.value(QStringLiteral("microstructure")).toObject().value(QStringLiteral("sources")).toArray();
    QStringList parts;
    for (const auto& value : sources) {
        const auto source = value.toObject();
        const auto age = source.value(QStringLiteral("age_ms"));
        const qint64 age_ms = age.isString() ? age.toString().toLongLong() : static_cast<qint64>(age.toDouble(-1));
        const QString name = source.value(QStringLiteral("source")).toString().toUpper();
        const QString status = source.value(QStringLiteral("status")).toString().toUpper();
        if (!name.isEmpty())
            parts << QStringLiteral("%1 %2 %3ms").arg(name, status, age_ms >= 0 ? QString::number(age_ms) : QStringLiteral("--"));
    }
    sources_value_->setText(parts.isEmpty() ? tr("No fresh cross-venue snapshot is available yet.")
                                              : parts.join(QStringLiteral("  |  ")));
}

void CryptoAutomationCockpit::refresh() {
    const auto state = read_json(daemon_file(QStringLiteral("scalp_state.json")));
    const auto config_file = read_json(daemon_file(QStringLiteral("scalp_engine.json")));
    const auto guard = read_json(daemon_file(QStringLiteral("automation_live_guard.json")));
    const auto config = state.value(QStringLiteral("config")).toObject(config_file);
    const auto decisions = state.value(QStringLiteral("decisions")).toArray();
    const auto decision = decisions.isEmpty() ? QJsonObject{} : decisions.first().toObject();
    const bool engine_running = state.value(QStringLiteral("status")).toString() == QLatin1String("running")
                                && config.value(QStringLiteral("enabled")).toBool();
    const auto expiry = QDateTime::fromString(guard.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    const bool live_armed = guard.value(QStringLiteral("enabled")).toBool()
                            && (!expiry.isValid() || expiry >= QDateTime::currentDateTimeUtc());

    venue_caption_->setText(tr("ACTIVE CRYPTO ACCOUNT"));
    set_metric(venue_value_, venue_caption_, venue_title(exchange_id_) + (is_paper_ ? tr(" / PAPER") : tr(" / LIVE")),
               is_paper_ ? colors::CYAN() : colors::POSITIVE());
    // ONE engine, two parameter styles (scalp|spot) — show what is actually
    // configured instead of the old hardcoded "SCALP ENGINE". Engines started
    // before the style field existed default to scalp (their preset).
    const QString style = config.value(QStringLiteral("style"))
                              .toString(config_file.value(QStringLiteral("style")).toString(QStringLiteral("scalp")))
                              .toUpper();
    engine_caption_->setText(style + tr(" ENGINE"));
    set_metric(engine_value_, engine_caption_,
               engine_running ? style + tr(" · RUNNING") : style + tr(" · OFFLINE"),
               engine_running ? colors::POSITIVE() : colors::NEGATIVE());
    guard_caption_->setText(tr("AUTOMATION GUARD"));
    set_metric(guard_value_, guard_caption_, live_armed ? tr("LIVE ARMED") : tr("PAPER ONLY"),
               live_armed ? colors::WARNING() : colors::CYAN());
    const double required_bps = decision.value(QStringLiteral("required_edge_bps")).toDouble();
    hurdle_caption_->setText(tr("MOVE REQUIRED"));
    set_metric(hurdle_value_, hurdle_caption_, required_bps > 0 ? bps_text(required_bps) : QStringLiteral("--"), colors::WARNING());

    const QString verdict = decision.value(QStringLiteral("verdict")).toString(tr("WAITING"));
    verdict_value_->setText(verdict);
    verdict_value_->setStyleSheet(QStringLiteral("color:%1;").arg(verdict.contains(QLatin1String("CANDIDATE")) ? colors::POSITIVE() : colors::WARNING()));
    direction_value_->setText(decision.value(QStringLiteral("direction")).toString(QStringLiteral("--")).toUpper());
    const double reference = decision.value(QStringLiteral("reference_price")).toDouble();
    price_value_->setText(reference > 0 ? QStringLiteral("$%1").arg(QString::number(reference, 'f', 2)) : QStringLiteral("--"));
    const double net_bps = decision.value(QStringLiteral("net_after_cost_bps")).toDouble();
    edge_value_->setText(decision.contains(QStringLiteral("net_after_cost_bps")) ? bps_text(net_bps) : QStringLiteral("--"));
    edge_value_->setStyleSheet(QStringLiteral("color:%1;").arg(net_bps > 0 ? colors::POSITIVE() : colors::NEGATIVE()));

    QStringList blockers;
    for (const auto& value : decision.value(QStringLiteral("blockers")).toArray()) blockers << value.toString();
    blockers_value_->setText(blockers.isEmpty() ? tr("No current engine blockers.") : blockers.join(QStringLiteral("  |  ")));
    render_sources(decision);
    heartbeat_value_->setText(age_text(state.value(QStringLiteral("heartbeat_at")).toString()));
    heartbeat_value_->setStyleSheet(QStringLiteral("color:%1;").arg(engine_running ? colors::POSITIVE() : colors::NEGATIVE()));
}

} // namespace openmarketterminal::screens::crypto
