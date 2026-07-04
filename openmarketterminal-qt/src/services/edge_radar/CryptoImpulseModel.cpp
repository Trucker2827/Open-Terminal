#include "services/edge_radar/CryptoImpulseModel.h"

#include <QDateTime>
#include <QJsonArray>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::edge_radar {

namespace latency = openmarketterminal::services::crypto_latency;

namespace {

double clamp01(double v) {
    if (!std::isfinite(v))
        return 0.0;
    return std::clamp(v, 0.0, 1.0);
}

QString pct(double value) {
    return QStringLiteral("%1%").arg(value, 0, 'f', 3);
}

} // namespace

CryptoImpulseModel::CryptoImpulseModel(CryptoImpulseOptions options)
    : options_(options) {}

void CryptoImpulseModel::clear() {
    ticks_.clear();
    latest_direct_tick_ms_ = 0;
}

void CryptoImpulseModel::add_tick(const latency::CryptoLatencyTick& tick) {
    if (tick.price <= 0.0 || tick.received_ts_ms <= 0)
        return;

    const bool advisory = tick.source == QStringLiteral("bitcointicker");
    if (!advisory)
        latest_direct_tick_ms_ = std::max(latest_direct_tick_ms_, tick.received_ts_ms);

    // Bitcointicker is useful as a website-lag reference, but a direct exchange
    // tick should drive the impulse when both are live.
    if (advisory && latest_direct_tick_ms_ > 0 && tick.received_ts_ms - latest_direct_tick_ms_ < 2000)
        return;

    ticks_.push_back(tick);
    std::stable_sort(ticks_.begin(), ticks_.end(), [](const auto& a, const auto& b) {
        if (a.received_ts_ms != b.received_ts_ms)
            return a.received_ts_ms < b.received_ts_ms;
        return a.sequence < b.sequence;
    });

    const qint64 newest = ticks_.isEmpty() ? 0 : ticks_.last().received_ts_ms;
    while (!ticks_.isEmpty() && newest - ticks_.first().received_ts_ms > 125000)
        ticks_.removeFirst();
}

CryptoImpulseWindow CryptoImpulseModel::window(int seconds) const {
    CryptoImpulseWindow out;
    out.seconds = seconds;
    if (ticks_.size() < 2 || seconds <= 0)
        return out;

    const auto& end = ticks_.last();
    const qint64 target = end.received_ts_ms - static_cast<qint64>(seconds) * 1000;
    int start_index = -1;
    for (int i = ticks_.size() - 1; i >= 0; --i) {
        if (ticks_[i].received_ts_ms <= target) {
            start_index = i;
            break;
        }
    }
    if (start_index < 0)
        start_index = 0;

    const auto& start = ticks_[start_index];
    const qint64 span = end.received_ts_ms - start.received_ts_ms;
    const qint64 required = std::max<qint64>(1500, static_cast<qint64>(seconds) * 600);
    if (start.price <= 0.0 || end.price <= 0.0 || span < required)
        return out;

    out.start_price = start.price;
    out.end_price = end.price;
    out.start_ts_ms = start.received_ts_ms;
    out.end_ts_ms = end.received_ts_ms;
    out.move_pct = ((end.price / start.price) - 1.0) * 100.0;
    out.velocity_pct_per_sec = span > 0 ? out.move_pct / (static_cast<double>(span) / 1000.0) : 0.0;
    out.available = true;
    return out;
}

CryptoImpulseSignal CryptoImpulseModel::signal(int primary_window_seconds) const {
    CryptoImpulseSignal out;
    out.symbol = ticks_.isEmpty() ? QStringLiteral("BTC-USD") : ticks_.last().symbol;
    out.windows = {window(5), window(15), window(60)};

    CryptoImpulseWindow primary;
    for (const auto& w : out.windows) {
        if (w.available && w.seconds == primary_window_seconds) {
            primary = w;
            break;
        }
    }
    if (!primary.available) {
        for (const auto& w : out.windows) {
            if (w.available) {
                primary = w;
                break;
            }
        }
    }

    if (!ticks_.isEmpty()) {
        const auto& latest = ticks_.last();
        out.freshest_source = latest.source;
        out.latest_tick_age_ms = QDateTime::currentMSecsSinceEpoch() - latest.received_ts_ms;
    }

    QStringList reject;
    if (ticks_.size() < 2 || !primary.available)
        reject << QStringLiteral("warming up price history");
    if (out.latest_tick_age_ms < 0 || out.latest_tick_age_ms > options_.stale_after_ms)
        reject << QStringLiteral("freshest tick is stale");

    if (primary.available) {
        const double abs_move = std::abs(primary.move_pct);
        out.direction = primary.move_pct > 0.0 ? QStringLiteral("up")
                                               : (primary.move_pct < 0.0 ? QStringLiteral("down")
                                                                        : QStringLiteral("flat"));
        out.strength = abs_move >= options_.strong_move_pct ? QStringLiteral("strong")
                      : abs_move >= options_.weak_move_pct ? QStringLiteral("weak")
                                                           : QStringLiteral("none");
        out.confidence = clamp01(abs_move / std::max(0.0001, options_.strong_move_pct));
        if (out.latest_tick_age_ms > options_.stale_after_ms / 2)
            out.confidence *= 0.75;
        if (out.strength == QStringLiteral("none"))
            reject << QStringLiteral("move below impulse threshold");
        if (out.confidence < options_.minimum_confidence)
            reject << QStringLiteral("confidence below gate");
        out.rationale = QStringLiteral("%1 %2s %3, velocity %4%/s, source %5")
                            .arg(out.symbol)
                            .arg(primary.seconds)
                            .arg(pct(primary.move_pct))
                            .arg(primary.velocity_pct_per_sec, 0, 'f', 4)
                            .arg(out.freshest_source.isEmpty() ? QStringLiteral("-") : out.freshest_source);
    }

    if (reject.isEmpty() && out.direction != QStringLiteral("flat")) {
        out.gate = QStringLiteral("pass");
        out.recommendation = out.strength == QStringLiteral("strong")
                                 ? QStringLiteral("candidate")
                                 : QStringLiteral("watch");
    } else {
        out.gate = QStringLiteral("reject");
        out.recommendation = QStringLiteral("no-trade");
    }
    out.rejection_reasons = reject.join(QStringLiteral("; "));
    if (out.rationale.isEmpty())
        out.rationale = out.rejection_reasons.isEmpty() ? QStringLiteral("no impulse") : out.rejection_reasons;
    return out;
}

double CryptoImpulseModel::anchor_probability(const CryptoImpulseSignal& signal) {
    CryptoImpulseWindow primary;
    for (const auto& w : signal.windows) {
        if (w.available && w.seconds == 15) {
            primary = w;
            break;
        }
    }
    if (!primary.available) {
        for (const auto& w : signal.windows) {
            if (w.available) {
                primary = w;
                break;
            }
        }
    }
    if (!primary.available || signal.gate != QStringLiteral("pass"))
        return 0.5;

    const double push = std::min(0.18, std::abs(primary.move_pct) / 1.0 * 0.18);
    if (signal.direction == QStringLiteral("down"))
        return 0.5 - push;
    if (signal.direction == QStringLiteral("up"))
        return 0.5 + push;
    return 0.5;
}

QJsonObject CryptoImpulseModel::to_json(const CryptoImpulseSignal& signal) {
    QJsonArray windows;
    for (const auto& w : signal.windows) {
        windows.append(QJsonObject{{"seconds", w.seconds},
                                   {"start_price", w.start_price},
                                   {"end_price", w.end_price},
                                   {"move_pct", w.move_pct},
                                   {"velocity_pct_per_sec", w.velocity_pct_per_sec},
                                   {"start_ts_ms", QString::number(w.start_ts_ms)},
                                   {"end_ts_ms", QString::number(w.end_ts_ms)},
                                   {"available", w.available}});
    }
    return QJsonObject{{"symbol", signal.symbol},
                       {"direction", signal.direction},
                       {"strength", signal.strength},
                       {"recommendation", signal.recommendation},
                       {"gate", signal.gate},
                       {"rationale", signal.rationale},
                       {"rejection_reasons", signal.rejection_reasons},
                       {"confidence", signal.confidence},
                       {"latest_tick_age_ms", QString::number(signal.latest_tick_age_ms)},
                       {"freshest_source", signal.freshest_source},
                       {"btc_anchor_probability", anchor_probability(signal)},
                       {"windows", windows}};
}

} // namespace openmarketterminal::services::edge_radar
