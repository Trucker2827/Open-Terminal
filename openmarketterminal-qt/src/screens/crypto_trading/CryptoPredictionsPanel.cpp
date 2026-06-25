#include "screens/crypto_trading/CryptoPredictionsPanel.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens::crypto {

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
}  // namespace

CryptoPredictionsPanel::CryptoPredictionsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoPredictionsPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(2);

    title_ = new QLabel(this);
    title_->setObjectName("cryptoPredictionsTitle");
    layout->addWidget(title_);

    subtitle_ = new QLabel(this);
    subtitle_->setObjectName("cryptoPredictionsSubtitle");
    layout->addWidget(subtitle_);

    status_ = new QLabel(this);
    status_->setObjectName("cryptoPredictionsStatus");
    layout->addWidget(status_);

    list_ = new QListWidget(this);
    list_->setObjectName("cryptoPredictionsList");
    list_->setAlternatingRowColors(true);
    list_->setWordWrap(true);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(list_, 1);

    // Coinbase predictions = Kalshi markets; bind to the Kalshi adapter.
    adapter_ = PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
    if (adapter_) {
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::events_ready, this,
                &CryptoPredictionsPanel::on_events_ready);
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::error_occurred, this,
                &CryptoPredictionsPanel::on_error);
    }

    retranslate();
}

void CryptoPredictionsPanel::retranslate() {
    title_->setText(tr("PREDICTIONS · COINBASE"));
    subtitle_->setText(tr("Crypto event contracts — powered by Kalshi"));
}

void CryptoPredictionsPanel::refresh() {
    if (!adapter_) {
        status_->setText(tr("Prediction markets unavailable"));
        return;
    }
    status_->setText(tr("Loading crypto markets…"));
    // "Crypto" is the recognized Kalshi category; the adapter resolves it to the
    // crypto series. We list EVENTS (not individual strike-markets) so each crypto
    // series — Bitcoin / Ethereum / Shiba Inu price, etc. — is one clean row
    // instead of dozens of near-identical strikes.
    adapter_->list_events(QStringLiteral("Crypto"), QStringLiteral("volume"), 100, 0);
}

void CryptoPredictionsPanel::on_events_ready(const QVector<PredictionEvent>& events) {
    QVector<PredictionEvent> crypto;
    crypto.reserve(events.size());
    for (const auto& e : events)
        if (!e.closed)  // category-resolved → already crypto; drop settled events
            crypto.append(e);

    std::sort(crypto.begin(), crypto.end(),
              [](const PredictionEvent& a, const PredictionEvent& b) { return a.volume > b.volume; });

    list_->clear();
    if (crypto.isEmpty()) {
        status_->setText(tr("No crypto prediction markets right now"));
        return;
    }
    status_->setText(tr("%1 crypto markets").arg(crypto.size()));

    for (const auto& e : crypto) {
        const int n = e.markets.size();
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n  %2 contracts   ·   Vol %3").arg(e.title).arg(n).arg(fmt_volume(e.volume)), list_);
        item->setToolTip(e.title);
    }
}

void CryptoPredictionsPanel::on_error(const QString& context, const QString& message) {
    status_->setText(tr("Error: %1").arg(message.isEmpty() ? context : message));
}

} // namespace openmarketterminal::screens::crypto
