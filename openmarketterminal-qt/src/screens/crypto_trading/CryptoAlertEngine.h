#pragma once
// Local crypto price/spread alerts (Task 12). Pure Qt-core engine so the
// firing rules are unit-testable; the screen owns persistence (Settings)
// and notification delivery.
//
// Rules: a cross alert needs a PRIOR tick on the other side of the threshold
// (the first observation only seeds — no fire-on-load); firing disarms the
// alert in place (one-shot until rearm); the disarmed state is part of the
// alerts() snapshot so persistence prevents a refire on restart.

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::crypto {

struct CryptoAlert {
    QString id;        // uuid string
    QString exchange;
    QString symbol;
    QString kind;      // "price_cross_up" | "price_cross_down" | "spread_bps"
    double threshold = 0; // price, or bps for spread
    bool armed = true;

    QJsonObject to_json() const {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("exchange"), exchange},
                           {QStringLiteral("symbol"), symbol},
                           {QStringLiteral("kind"), kind},
                           {QStringLiteral("threshold"), threshold},
                           {QStringLiteral("armed"), armed}};
    }
    static CryptoAlert from_json(const QJsonObject& j) {
        CryptoAlert a;
        a.id = j.value(QStringLiteral("id")).toString();
        a.exchange = j.value(QStringLiteral("exchange")).toString();
        a.symbol = j.value(QStringLiteral("symbol")).toString();
        a.kind = j.value(QStringLiteral("kind")).toString();
        a.threshold = j.value(QStringLiteral("threshold")).toDouble();
        a.armed = j.value(QStringLiteral("armed")).toBool(true);
        return a;
    }
};

class CryptoAlertEngine {
  public:
    void set_alerts(const QVector<CryptoAlert>& alerts) { alerts_ = alerts; }
    QVector<CryptoAlert> alerts() const { return alerts_; }
    void add(const CryptoAlert& a) { alerts_.append(a); }
    void remove(const QString& id) {
        alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                     [&](const CryptoAlert& a) { return a.id == id; }),
                      alerts_.end());
    }
    void rearm(const QString& id) {
        for (auto& a : alerts_)
            if (a.id == id)
                a.armed = true;
    }

    /// Evaluate one tick; returns the alerts that FIRE (disarming them in
    /// place). last <= 0 skips cross checks; bid/ask <= 0 skips spread checks.
    QVector<CryptoAlert> on_tick(const QString& exchange, const QString& symbol,
                                 double last, double bid, double ask) {
        QVector<CryptoAlert> fired;
        const QString key = exchange + QLatin1Char(':') + symbol;
        const double prev = last_price_.value(key, 0.0);
        for (auto& a : alerts_) {
            if (!a.armed || a.exchange != exchange || a.symbol != symbol)
                continue;
            bool fire = false;
            if (a.kind == QLatin1String("price_cross_up")) {
                fire = last > 0 && prev > 0 && prev < a.threshold && last >= a.threshold;
            } else if (a.kind == QLatin1String("price_cross_down")) {
                fire = last > 0 && prev > 0 && prev > a.threshold && last <= a.threshold;
            } else if (a.kind == QLatin1String("spread_bps")) {
                if (bid > 0 && ask > bid) {
                    const double mid = (bid + ask) / 2.0;
                    fire = (ask - bid) / mid * 10000.0 >= a.threshold;
                }
            }
            if (fire) {
                a.armed = false;
                fired.append(a);
            }
        }
        if (last > 0)
            last_price_.insert(key, last);
        return fired;
    }

  private:
    QVector<CryptoAlert> alerts_;
    QHash<QString, double> last_price_; // exchange:symbol → previous last (cross detection)
};

} // namespace openmarketterminal::crypto
