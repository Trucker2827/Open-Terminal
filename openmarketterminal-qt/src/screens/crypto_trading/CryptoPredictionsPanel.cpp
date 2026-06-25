#include "screens/crypto_trading/CryptoPredictionsPanel.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens::crypto {

using openmarketterminal::services::prediction::PredictionExchangeRegistry;
using openmarketterminal::services::prediction::PredictionMarket;

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
        connect(adapter_, &services::prediction::PredictionExchangeAdapter::markets_ready, this,
                &CryptoPredictionsPanel::on_markets_ready);
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
    // crypto series, so the returned markets are already the crypto subset.
    adapter_->list_markets(QStringLiteral("Crypto"), QStringLiteral("volume"), 100, 0);
}

void CryptoPredictionsPanel::on_markets_ready(const QVector<PredictionMarket>& markets) {
    QVector<PredictionMarket> crypto;
    crypto.reserve(markets.size());
    for (const auto& m : markets)
        if (!m.closed)  // category-resolved → already crypto; just drop settled markets
            crypto.append(m);

    std::sort(crypto.begin(), crypto.end(),
              [](const PredictionMarket& a, const PredictionMarket& b) { return a.volume > b.volume; });

    list_->clear();
    if (crypto.isEmpty()) {
        status_->setText(tr("No crypto prediction markets right now"));
        return;
    }
    status_->setText(tr("%1 crypto markets").arg(crypto.size()));

    for (const auto& m : crypto) {
        // "Yes" probability (first outcome) shown in cents — the standard
        // prediction-market price display.
        double yes = m.outcomes.isEmpty() ? 0.0 : m.outcomes.first().price;
        const QString yes_c = QStringLiteral("%1¢").arg(yes * 100.0, 0, 'f', 0);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n  Yes %2   ·   Vol %3").arg(m.question, yes_c, fmt_volume(m.volume)), list_);
        item->setToolTip(m.question);
    }
}

void CryptoPredictionsPanel::on_error(const QString& context, const QString& message) {
    status_->setText(tr("Error: %1").arg(message.isEmpty() ? context : message));
}

} // namespace openmarketterminal::screens::crypto
