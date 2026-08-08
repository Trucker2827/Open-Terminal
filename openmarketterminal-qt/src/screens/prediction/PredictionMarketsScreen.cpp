#include "screens/prediction/PredictionMarketsScreen.h"

#include "screens/kalshi/KalshiScreen.h"
#include "screens/polymarket/PolymarketScreen.h"
#include "services/prediction/PredictionExchangeRegistry.h"

#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSizePolicy>

namespace openmarketterminal::screens {

PredictionMarketsScreen::PredictionMarketsScreen(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    // Kalshi is the default Predictions workspace. Polymarket is constructed
    // on first switch so opening the dock does not pay for a venue the
    // operator may never open.
    kalshi_ = new kalshi::KalshiScreen(stack_);
    stack_->addWidget(kalshi_);
    layout->addWidget(stack_);

    connect(kalshi_, &kalshi::KalshiScreen::venue_switch_requested,
            this, &PredictionMarketsScreen::show_venue);

    const QString active = services::prediction::PredictionExchangeRegistry::instance().active_id();
    show_venue(active == QStringLiteral("polymarket") ? active : QStringLiteral("kalshi"));
}

QSize PredictionMarketsScreen::sizeHint() const { return QSize(1120, 800); }

QSize PredictionMarketsScreen::minimumSizeHint() const { return QSize(760, 560); }

QVariantMap PredictionMarketsScreen::save_state() const { return {{QStringLiteral("venue"), venue_}}; }

void PredictionMarketsScreen::restore_state(const QVariantMap& state) {
    show_venue(state.value(QStringLiteral("venue"), QStringLiteral("kalshi")).toString());
}

void PredictionMarketsScreen::ensure_polymarket() {
    if (polymarket_) return;
    polymarket_ = new PolymarketScreen(stack_);
    stack_->addWidget(polymarket_);
    connect(polymarket_, &PolymarketScreen::venue_switch_requested,
            this, &PredictionMarketsScreen::show_venue);
}

void PredictionMarketsScreen::show_venue(const QString& venue) {
    venue_ = venue == QStringLiteral("polymarket") ? QStringLiteral("polymarket") : QStringLiteral("kalshi");
    services::prediction::PredictionExchangeRegistry::instance().set_active(venue_);
    if (venue_ == QStringLiteral("polymarket")) {
        ensure_polymarket();
        stack_->setCurrentWidget(polymarket_);
    } else {
        stack_->setCurrentWidget(kalshi_);
    }
}

} // namespace openmarketterminal::screens
