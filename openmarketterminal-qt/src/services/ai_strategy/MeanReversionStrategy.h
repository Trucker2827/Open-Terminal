#pragma once
// MeanReversionStrategy.h — deterministic mean-reversion reference strategy
// (Paper Strategy-Loop Driver, Task 2).
//
// A minimal, fully deterministic Strategy used to EXERCISE the loop + audit
// trail end-to-end. It is NOT meant to have edge: it buys a small clip on a dip
// below a rolling mean and sells-to-close on a rip above it, tracking its own
// holdings purely from actual fills. No I/O, no tool calls — given the same
// snapshot sequence it always proposes the same intents.
#include "services/ai_strategy/Strategy.h"

#include <QList>
#include <QMap>
#include <QSet>

namespace openmarketterminal::ai_strategy {

class MeanReversionStrategy : public Strategy {
  public:
    /// `window` rolling-mean length, `band` fractional threshold around the mean,
    /// `qty` clip size per order.
    MeanReversionStrategy(QStringList symbols, int window = 5, double band = 0.01,
                          double qty = 10.0);

    QString name() const override { return QStringLiteral("meanrev"); }
    QStringList universe() const override { return symbols_; }
    QVector<TradeIntent> propose(const MarketSnapshot& s) override;
    void on_fill(const TradeIntent& intent, const QJsonObject& submit_result) override;

  private:
    QStringList symbols_;
    int window_;
    double band_;
    double qty_;
    QMap<QString, QList<double>> prices_;  ///< symbol → recent prices (≤ window_).
    QSet<QString> holding_;                ///< symbols currently held (from fills).
};

} // namespace openmarketterminal::ai_strategy
