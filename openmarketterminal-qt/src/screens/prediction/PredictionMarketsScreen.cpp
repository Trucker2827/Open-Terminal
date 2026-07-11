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
    polymarket_ = new PolymarketScreen(stack_);
    kalshi_ = new kalshi::KalshiScreen(stack_);
    stack_->addWidget(kalshi_);
    // Kalshi is added first so constructing the shell never briefly shows the
    // legacy Polymarket workspace and triggers an unfiltered adapter request.
    stack_->addWidget(polymarket_);
    layout->addWidget(stack_);

    connect(polymarket_, &PolymarketScreen::venue_switch_requested,
            this, &PredictionMarketsScreen::show_venue);
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

void PredictionMarketsScreen::show_venue(const QString& venue) {
    venue_ = venue == QStringLiteral("polymarket") ? QStringLiteral("polymarket") : QStringLiteral("kalshi");
    services::prediction::PredictionExchangeRegistry::instance().set_active(venue_);
    stack_->setCurrentWidget(venue_ == QStringLiteral("polymarket")
                                 ? static_cast<QWidget*>(polymarket_)
                                 : static_cast<QWidget*>(kalshi_));
}

} // namespace openmarketterminal::screens
