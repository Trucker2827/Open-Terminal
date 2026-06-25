#include "screens/crypto_trading/CryptoPredictionsPanel.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens::crypto {

using openmarketterminal::services::prediction::crypto_event_priority;
using openmarketterminal::services::prediction::PredictionEvent;
using openmarketterminal::services::prediction::PredictionExchangeRegistry;

namespace {
QString fmt_volume(double v) {
    if (v >= 1e9)
        return QStringLiteral("$%1B").arg(v / 1e9, 0, 'f', 1);
    if (v >= 1e6)
        return QStringLiteral("$%1M").arg(v / 1e6, 0, 'f', 1);
    if (v >= 1e3)
        return QStringLiteral("$%1K").arg(v / 1e3, 0, 'f', 1);
    return QStringLiteral("$%1").arg(v, 0, 'f', 0);
}

QPushButton* make_pill(const QString& text) {
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    b->setProperty("class", "predPill");
    return b;
}
}  // namespace

CryptoPredictionsPanel::CryptoPredictionsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoPredictionsPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(3);

    // Single honest header: it's Kalshi's order book, also distributed by Coinbase.
    title_ = new QLabel(tr("PREDICTIONS · CRYPTO"), this);
    title_->setObjectName("cryptoPredictionsTitle");
    layout->addWidget(title_);

    subtitle_ = new QLabel(tr("Kalshi event contracts — also offered via Coinbase"), this);
    subtitle_->setObjectName("cryptoPredictionsSubtitle");
    subtitle_->setWordWrap(true);
    layout->addWidget(subtitle_);

    // ── Cadence filter: 15m / 1h / Daily ──
    auto* cad = new QHBoxLayout;
    cad->setSpacing(4);
    cad_15m_ = make_pill(tr("15m"));
    cad_1h_ = make_pill(tr("1h"));
    cad_daily_ = make_pill(tr("Daily"));
    cad->addWidget(cad_15m_);
    cad->addWidget(cad_1h_);
    cad->addWidget(cad_daily_);
    cad->addStretch(1);
    layout->addLayout(cad);

    auto* cad_group = new QButtonGroup(this);
    cad_group->setExclusive(true);
    cad_group->addButton(cad_15m_);
    cad_group->addButton(cad_1h_);
    cad_group->addButton(cad_daily_);
    cad_daily_->setChecked(true);
    connect(cad_15m_, &QPushButton::clicked, this, [this]() { set_cadence(QStringLiteral("fifteen_min")); });
    connect(cad_1h_, &QPushButton::clicked, this, [this]() { set_cadence(QStringLiteral("hourly")); });
    connect(cad_daily_, &QPushButton::clicked, this, [this]() { set_cadence(QString()); });

    status_ = new QLabel(this);
    status_->setObjectName("cryptoPredictionsStatus");
    layout->addWidget(status_);

    list_ = new QListWidget(this);
    list_->setObjectName("cryptoPredictionsList");
    list_->setAlternatingRowColors(true);
    list_->setWordWrap(true);
    list_->setCursor(Qt::PointingHandCursor);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setToolTip(tr("Click a market to bet on the Predictions screen"));
    // Clicking a market opens the full Predictions screen to place the bet.
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { emit bet_requested(); });
    layout->addWidget(list_, 1);

    adapter_ = PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
    if (adapter_) {
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::events_ready, this,
                &CryptoPredictionsPanel::on_events_ready);
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::error_occurred, this,
                &CryptoPredictionsPanel::on_error);
    }
}

void CryptoPredictionsPanel::set_cadence(const QString& freq) {
    if (freq == cadence_)
        return;
    cadence_ = freq;
    refresh();
}

void CryptoPredictionsPanel::refresh() {
    if (!adapter_) {
        status_->setText(tr("Prediction markets unavailable"));
        return;
    }
    status_->setText(tr("Loading crypto markets…"));
    // "Crypto" resolves to the Kalshi crypto series; the optional cadence suffix
    // (Crypto@fifteen_min / Crypto@hourly) narrows to a short-term frequency.
    // List EVENTS so each series is one row, not every strike.
    QString category = QStringLiteral("Crypto");
    if (!cadence_.isEmpty())
        category += QLatin1Char('@') + cadence_;
    adapter_->list_events(category, QStringLiteral("volume"), 100, 0);
}

void CryptoPredictionsPanel::on_events_ready(const QVector<PredictionEvent>& events) {
    QVector<PredictionEvent> crypto;
    crypto.reserve(events.size());
    for (const auto& e : events)
        if (!e.closed)
            crypto.append(e);

    // Majors (BTC/ETH/SOL/…) first, then by volume — so it isn't just whatever
    // token's price ladder Kalshi returns at the top.
    std::sort(crypto.begin(), crypto.end(), [](const PredictionEvent& a, const PredictionEvent& b) {
        const int pa = crypto_event_priority(a.title);
        const int pb = crypto_event_priority(b.title);
        return pa != pb ? pa < pb : a.volume > b.volume;
    });

    list_->clear();
    if (crypto.isEmpty()) {
        status_->setText(tr("No crypto prediction markets right now"));
        return;
    }
    status_->setText(tr("%1 crypto markets").arg(crypto.size()));

    for (const auto& e : crypto) {
        const qsizetype n = e.markets.size();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n  %2 contracts   ·   Vol %3").arg(e.title).arg(n).arg(fmt_volume(e.volume)), list_);
        item->setToolTip(e.title);
    }
}

void CryptoPredictionsPanel::on_error(const QString& context, const QString& message) {
    status_->setText(tr("Error: %1").arg(message.isEmpty() ? context : message));
}

} // namespace openmarketterminal::screens::crypto
