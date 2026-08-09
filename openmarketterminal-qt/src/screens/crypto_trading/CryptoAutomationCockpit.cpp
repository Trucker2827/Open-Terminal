#include "screens/crypto_trading/CryptoAutomationCockpit.h"

#include "core/config/ProfileManager.h"
#include "mcp/tools/SettingsGate.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens::crypto {

namespace {

using namespace openmarketterminal::ui;
namespace mcp = openmarketterminal::mcp;

QString role_color(const QString& role) {
    if (role == QLatin1String("green")) return colors::POSITIVE();
    if (role == QLatin1String("red")) return colors::NEGATIVE();
    if (role == QLatin1String("amber")) return colors::WARNING();
    if (role == QLatin1String("cyan")) return colors::CYAN();
    return colors::TEXT_SECONDARY();
}

QString daemon_file(const QString& name) {
    return openmarketterminal::ProfileManager::instance().profile_root() + QStringLiteral("/daemon/") +
           name;
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

QString tape_line(const CryptoCockpitTapeRow& row) {
    const QString net =
        row.net_known ? crypto_cockpit_bps_text(row.net_bps) : QStringLiteral("--");
    const QString venue =
        row.selected_venue.isEmpty() ? QStringLiteral("--") : row.selected_venue.toUpper();
    const QString liq = row.liquidity.isEmpty() ? QStringLiteral("--") : row.liquidity.toUpper();
    const QString why =
        row.blockers.isEmpty() ? QStringLiteral("clear") : row.blockers.join(QStringLiteral(", "));
    return QStringLiteral("%1  %2  %3  %4  net %5  → %6  · %7")
        .arg(row.symbol.isEmpty() ? QStringLiteral("—") : row.symbol, row.verdict, row.direction, liq,
             net, venue, why);
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

    auto* mood = new QFrame;
    mood->setObjectName("cryptoCockpitPanel");
    auto* mood_layout = new QVBoxLayout(mood);
    mood_layout->setContentsMargins(10, 6, 10, 6);
    mood_layout->setSpacing(2);
    mood_value_ = text_label("cryptoCockpitMetricValue", tr("PAPER SCALP"));
    mood_detail_ = text_label("cryptoCockpitDetail", "--");
    mood_detail_->setWordWrap(true);
    mood_layout->addWidget(mood_value_);
    mood_layout->addWidget(mood_detail_);
    layout->addWidget(mood);

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
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(3);
    decide_title_ = text_label("cryptoCockpitSection", tr("DECIDE — LATEST ENVELOPE"));
    grid->addWidget(decide_title_, 0, 0, 1, 6);
    const auto field = [&grid](int column, const QString& name, QLabel*& value) {
        grid->addWidget(text_label("cryptoCockpitField", name), 1, column);
        value = text_label("cryptoCockpitFieldValue", "--");
        grid->addWidget(value, 2, column);
        grid->setColumnStretch(column, 1);
    };
    field(0, tr("SYMBOL"), symbol_value_);
    field(1, tr("VERDICT"), verdict_value_);
    field(2, tr("DIRECTION"), direction_value_);
    field(3, tr("LIQUIDITY"), liquidity_value_);
    field(4, tr("REQUIRED"), required_value_);
    field(5, tr("NET AFTER COST"), edge_value_);
    grid->addWidget(text_label("cryptoCockpitField", tr("REFERENCE")), 3, 0);
    price_value_ = text_label("cryptoCockpitFieldValue", "--");
    grid->addWidget(price_value_, 3, 1, 1, 2);
    grid->addWidget(text_label("cryptoCockpitField", tr("WHY THE ENGINE WAITED OR ACTED")), 4, 0, 1,
                    6);
    blockers_value_ = text_label("cryptoCockpitDetail", "--");
    blockers_value_->setWordWrap(true);
    grid->addWidget(blockers_value_, 5, 0, 1, 6);
    layout->addWidget(decision);

    auto* tape = new QFrame;
    tape->setObjectName("cryptoCockpitPanel");
    auto* tape_layout = new QVBoxLayout(tape);
    tape_layout->setContentsMargins(10, 6, 10, 6);
    tape_layout->setSpacing(3);
    tape_layout->addWidget(text_label("cryptoCockpitSection", tr("TAPE — RECENT OPPORTUNITIES")));
    tape_census_ = text_label("cryptoCockpitDetail", "--");
    tape_layout->addWidget(tape_census_);
    tape_list_ = new QListWidget;
    tape_list_->setObjectName("cryptoCockpitTape");
    tape_list_->setMinimumHeight(88);
    tape_list_->setMaximumHeight(140);
    tape_list_->setSelectionMode(QAbstractItemView::NoSelection);
    tape_list_->setFocusPolicy(Qt::NoFocus);
    tape_layout->addWidget(tape_list_);
    layout->addWidget(tape);

    auto* proof_panel = new QFrame;
    proof_panel->setObjectName("cryptoCockpitPanel");
    auto* proof_grid = new QGridLayout(proof_panel);
    proof_grid->setContentsMargins(10, 7, 10, 7);
    proof_grid->setHorizontalSpacing(18);
    proof_grid->setVerticalSpacing(3);
    proof_grid->addWidget(
        text_label("cryptoCockpitSection", tr("SCALP SHADOW PROOF — crypto-scalp-qualification-v1")),
        0, 0, 1, 6);
    const QStringList proof_headers{tr("SCOPE"), tr("STATE"), tr("SAMPLE"), tr("MEAN NET"),
                                    tr("WIN RATE"), tr("COVERAGE / CI")};
    for (int column = 0; column < proof_headers.size(); ++column) {
        proof_grid->addWidget(text_label("cryptoCockpitField", proof_headers[column]), 1, column);
        proof_grid->setColumnStretch(column, 1);
    }
    proof_symbol_row_ = make_proof_row(proof_grid, 2);
    proof_all_row_ = make_proof_row(proof_grid, 3);
    proof_status_ = text_label("cryptoCockpitDetail", "--");
    proof_status_->setWordWrap(true);
    proof_grid->addWidget(proof_status_, 4, 0, 1, 6);
    layout->addWidget(proof_panel);

    auto* qualify = new QFrame;
    qualify->setObjectName("cryptoCockpitPanel");
    auto* qualify_layout = new QVBoxLayout(qualify);
    qualify_layout->setContentsMargins(10, 6, 10, 6);
    qualify_layout->setSpacing(2);
    qualify_layout->addWidget(
        text_label("cryptoCockpitSection", tr("QUALIFICATION — crypto-scalp-qualification-v1")));
    qualification_value_ = text_label("cryptoCockpitMetricValue", tr("UNAVAILABLE"));
    qualification_detail_ = text_label("cryptoCockpitDetail", "--");
    qualification_detail_->setWordWrap(true);
    qualify_layout->addWidget(qualification_value_);
    qualify_layout->addWidget(qualification_detail_);
    layout->addWidget(qualify);

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
      #cryptoCockpitTape { background:%7; border:none; color:%5; font-family:%3; font-size:11px; }
      #cryptoCockpitTape::item { padding:2px 0; }
    )")
                      .arg(colors::BG_BASE(), colors::ORANGE(), fonts::DATA_FAMILY(),
                           colors::TEXT_SECONDARY(), colors::TEXT_PRIMARY(), colors::BG_HOVER(),
                           colors::BG_SURFACE(), colors::BORDER_DIM(), colors::TEXT_TERTIARY()));

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
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object()
                                                                          : QJsonObject{};
}

qint64 CryptoAutomationCockpit::file_age_ms(const QString& path, qint64 now_ms) {
    const QFileInfo info(path);
    if (!info.exists()) return -1;
    return now_ms - info.lastModified().toUTC().toMSecsSinceEpoch();
}

CryptoCockpitInputs CryptoAutomationCockpit::build_inputs(qint64 now_ms) const {
    CryptoCockpitInputs inputs;
    inputs.exchange_id = exchange_id_;
    inputs.is_paper = is_paper_;
    inputs.now_ms = now_ms;
    inputs.scalp_state = read_json(daemon_file(QStringLiteral("scalp_state.json")));
    inputs.scalp_engine = read_json(daemon_file(QStringLiteral("scalp_engine.json")));
    inputs.live_guard = read_json(daemon_file(QStringLiteral("automation_live_guard.json")));
    const QString qualify_path = daemon_file(QStringLiteral("scalp_qualification_v1.json"));
    inputs.qualification = read_json(qualify_path);
    inputs.qualification_age_ms = file_age_ms(qualify_path, now_ms);
    inputs.security.kill_switch = mcp::cli_kill_switch_engaged();
    inputs.security.cli_trading_allowed = mcp::cli_trading_allowed();
    inputs.security.cli_live_armed = mcp::cli_live_armed();
    inputs.security.cli_fast_live_armed = mcp::cli_fast_live_armed();
    inputs.security.venue_allowed = mcp::cli_venue_allowed(exchange_id_);
    inputs.security.allowed_venues = mcp::cli_allowed_venues();
    return inputs;
}

void CryptoAutomationCockpit::set_metric(QLabel* value, QLabel* caption, const QString& text,
                                         const QString& color) {
    value->setText(text);
    value->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    Q_UNUSED(caption);
}

void CryptoAutomationCockpit::bind_scene(const CryptoCockpitScene& scene) {
    mood_value_->setText(scene.mood);
    mood_value_->setStyleSheet(QStringLiteral("color:%1;").arg(role_color(scene.mood_role)));
    mood_detail_->setText(scene.mood_detail);

    venue_caption_->setText(tr("ACTIVE CRYPTO ACCOUNT"));
    set_metric(venue_value_, venue_caption_, scene.venue_line, role_color(scene.venue_role));
    engine_caption_->setText(scene.style + tr(" ENGINE"));
    set_metric(engine_value_, engine_caption_, scene.engine_line, role_color(scene.engine_role));
    guard_caption_->setText(tr("SCALP CANARY"));
    set_metric(guard_value_, guard_caption_, scene.mood, role_color(scene.mood_role));
    if (guard_value_) {
        guard_value_->setToolTip(
            tr("Spot/scalp live canary for Coinbase/Kraken automation — not Kalshi session arm. "
               "Arm the canary from Profile; Kalshi live arm stays in Predictions."));
    }
    if (guard_caption_) {
        guard_caption_->setToolTip(
            tr("Spot/scalp live canary for Coinbase/Kraken automation — not Kalshi session arm."));
    }
    hurdle_caption_->setText(tr("MOVE REQUIRED"));
    set_metric(hurdle_value_, hurdle_caption_, scene.hurdle_line, colors::WARNING());

    decide_title_->setText(scene.decide_symbol.isEmpty()
                               ? tr("DECIDE — LATEST ENVELOPE")
                               : tr("DECIDE — %1").arg(scene.decide_symbol));
    symbol_value_->setText(scene.decide_symbol.isEmpty() ? QStringLiteral("--")
                                                         : scene.decide_symbol);
    verdict_value_->setText(scene.decide_verdict.isEmpty() ? QStringLiteral("WAIT")
                                                          : scene.decide_verdict);
    verdict_value_->setStyleSheet(QStringLiteral("color:%1;").arg(role_color(scene.decide_role)));
    direction_value_->setText(scene.decide_direction);
    liquidity_value_->setText(scene.decide_liquidity);
    required_value_->setText(scene.decide_required);
    price_value_->setText(scene.decide_reference);
    edge_value_->setText(scene.decide_net);
    edge_value_->setStyleSheet(QStringLiteral("color:%1;")
                                   .arg(scene.decide_net.startsWith(QLatin1Char('-'))
                                            ? colors::NEGATIVE()
                                            : colors::POSITIVE()));
    blockers_value_->setText(scene.decide_blockers);

    tape_census_->setText(scene.tape_census);
    tape_list_->clear();
    for (const auto& row : scene.tape) tape_list_->addItem(tape_line(row));
    if (scene.tape.isEmpty()) tape_list_->addItem(tr("No daemon decisions yet."));

    sources_value_->setText(scene.feeds_line);
    heartbeat_value_->setText(scene.heartbeat);
    heartbeat_value_->setStyleSheet(
        QStringLiteral("color:%1;").arg(role_color(scene.heartbeat_role)));

    qualification_value_->setText(scene.qualification_state);
    qualification_value_->setStyleSheet(
        QStringLiteral("color:%1;").arg(role_color(scene.qualification_role)));
    qualification_detail_->setText(scene.qualification_detail);

    render_proof_row(proof_symbol_row_, scene.proof_symbol);
    render_proof_row(proof_all_row_, scene.proof_all);
    proof_status_->setText(scene.proof_status);
}

void CryptoAutomationCockpit::refresh() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    bind_scene(present_crypto_cockpit(build_inputs(now_ms)));
}

CryptoAutomationCockpit::ProofRow CryptoAutomationCockpit::make_proof_row(QGridLayout* grid,
                                                                         int grid_row) {
    ProofRow row;
    const auto cell = [&](int column, QLabel*& label) {
        label = text_label("cryptoCockpitFieldValue", "--");
        grid->addWidget(label, grid_row, column);
    };
    cell(0, row.scope);
    cell(1, row.verdict);
    cell(2, row.sample);
    cell(3, row.mean_net);
    cell(4, row.win_rate);
    cell(5, row.coverage);
    return row;
}

void CryptoAutomationCockpit::render_proof_row(ProofRow& row, const CryptoCockpitProofRow& proof) {
    row.scope->setText(proof.scope);
    row.verdict->setText(proof.verdict);
    row.verdict->setStyleSheet(QStringLiteral("color:%1;").arg(role_color(proof.verdict_role)));
    row.sample->setText(proof.sample);
    row.mean_net->setText(proof.mean_net);
    if (proof.has_metrics && proof.mean_net != QLatin1String("--")) {
        row.mean_net->setStyleSheet(QStringLiteral("color:%1;")
                                        .arg(proof.mean_net.startsWith(QLatin1Char('-'))
                                                 ? colors::NEGATIVE()
                                                 : colors::POSITIVE()));
    } else {
        row.mean_net->setStyleSheet({});
    }
    row.win_rate->setText(proof.win_rate);
    row.coverage->setText(proof.coverage);
}

} // namespace openmarketterminal::screens::crypto
