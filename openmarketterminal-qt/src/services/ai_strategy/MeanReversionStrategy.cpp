// MeanReversionStrategy.cpp — see MeanReversionStrategy.h.
#include "services/ai_strategy/MeanReversionStrategy.h"

namespace openmarketterminal::ai_strategy {

MeanReversionStrategy::MeanReversionStrategy(QStringList symbols, int window, double band,
                                             double qty)
    : symbols_(std::move(symbols)), window_(window), band_(band), qty_(qty) {}

QVector<TradeIntent> MeanReversionStrategy::propose(const MarketSnapshot& s) {
    QVector<TradeIntent> intents;
    for (const QString& sym : symbols_) {
        auto qit = s.quotes.find(sym);
        if (qit == s.quotes.end())
            continue;  // no quote this tick.
        const double price = qit.value();

        QList<double>& hist = prices_[sym];
        hist.append(price);
        while (hist.size() > window_)
            hist.removeFirst();
        if (hist.size() < window_)
            continue;  // warming up.

        double sum = 0.0;
        for (double p : hist)
            sum += p;
        const double mean = sum / hist.size();

        if (price < mean * (1.0 - band_) && !holding_.contains(sym)) {
            intents.append(TradeIntent{{"symbol", sym},
                                       {"side", "buy"},
                                       {"quantity", qty_},
                                       {"order_type", "limit"},
                                       {"limit_price", price}});
        } else if (price > mean * (1.0 + band_) && holding_.contains(sym)) {
            intents.append(TradeIntent{{"symbol", sym},
                                       {"side", "sell"},
                                       {"quantity", qty_},
                                       {"order_type", "limit"},
                                       {"limit_price", price}});
        }
    }
    return intents;
}

void MeanReversionStrategy::on_fill(const TradeIntent& intent, const QJsonObject& submit_result) {
    if (submit_result.value("status").toString() != QLatin1String("filled"))
        return;  // only actual fills move holdings.
    const QString sym = intent.value("symbol").toString();
    const QString side = intent.value("side").toString();
    if (side == QLatin1String("buy"))
        holding_.insert(sym);
    else if (side == QLatin1String("sell"))
        holding_.remove(sym);
}

} // namespace openmarketterminal::ai_strategy
