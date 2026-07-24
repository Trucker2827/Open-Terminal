#pragma once

#include "services/crypto_latency/CryptoLatencyService.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openmarketterminal::services::crypto_scalp {

// A deliberately small, deterministic signal surface.  Bars are immutable:
// build_closed_bars() always discards the newest (still-forming) bucket.  A
// signal can therefore never move or disappear because another tick arrived in
// the current bucket.
struct ClosedBar {
    qint64 bucket_ms = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    int ticks = 0;
};

struct ReversalSignal {
    QString call = QStringLiteral("ABSTAIN"); // BUY, SELL, ABSTAIN
    QString structure = QStringLiteral("NONE"); // BOS, CHOCH, NONE
    qint64 signal_bar_ms = 0;
    double reference_price = 0.0;
    double atr_bps = 0.0;
    double expected_move_bps = 0.0;
    double confidence = 0.0;
    double confirmed_swing_high = 0.0;
    double confirmed_swing_low = 0.0;
    QString reason;
};

struct VenueCost {
    QString venue;
    QString source;
    double maker_bps = 0.0;
    double taker_bps = 0.0;
    double slippage_bps = 0.0;
    double safety_bps = 0.0;
};

struct VenueProposal {
    QString venue;
    QString source;
    QString side;
    QString liquidity;
    QString time_in_force;
    double limit_price = 0.0;
    double bid = 0.0;
    double ask = 0.0;
    double spread_bps = 0.0;
    double entry_fee_bps = 0.0;
    double exit_fee_bps = 0.0;
    double estimated_slippage_bps = 0.0;
    double round_trip_cost_bps = 0.0;
    double expected_move_bps = 0.0;
    double net_edge_bps = 0.0;
    qint64 quote_age_ms = -1;
    qint64 ttl_ms = 0;
    bool executable = false;
    QStringList blockers;
};

inline QVector<ClosedBar> build_closed_bars(
    const QVector<crypto_latency::CryptoLatencyTick>& ticks,
    qint64 interval_ms = 1000) {
    QMap<qint64, ClosedBar> by_bucket;
    qint64 newest_bucket = std::numeric_limits<qint64>::min();
    for (const auto& tick : ticks) {
        const qint64 ts = tick.exchange_ts_ms > 0 ? tick.exchange_ts_ms : tick.received_ts_ms;
        if (ts <= 0 || interval_ms <= 0 || tick.price <= 0.0)
            continue;
        const qint64 bucket = (ts / interval_ms) * interval_ms;
        newest_bucket = std::max(newest_bucket, bucket);
        auto& bar = by_bucket[bucket];
        if (bar.ticks == 0) {
            bar.bucket_ms = bucket;
            bar.open = bar.high = bar.low = bar.close = tick.price;
        } else {
            bar.high = std::max(bar.high, tick.price);
            bar.low = std::min(bar.low, tick.price);
            bar.close = tick.price;
        }
        ++bar.ticks;
    }
    QVector<ClosedBar> out;
    out.reserve(by_bucket.size());
    for (auto it = by_bucket.cbegin(); it != by_bucket.cend(); ++it) {
        if (it.key() != newest_bucket)
            out.push_back(it.value());
    }
    return out;
}

inline double ema_close(const QVector<ClosedBar>& bars, int period) {
    if (bars.isEmpty() || period <= 0)
        return 0.0;
    const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
    double ema = bars.first().close;
    for (int i = 1; i < bars.size(); ++i)
        ema = alpha * bars[i].close + (1.0 - alpha) * ema;
    return ema;
}

inline ReversalSignal evaluate_reversal(
    const QVector<ClosedBar>& bars,
    int pivot_left = 2,
    int pivot_right = 2,
    int atr_period = 8) {
    ReversalSignal out;
    const int required = std::max(atr_period + 1, pivot_left + pivot_right + 3);
    if (bars.size() < required) {
        out.reason = QStringLiteral("INSUFFICIENT_CLOSED_BARS");
        return out;
    }

    const int last = bars.size() - 1;
    double swing_high = 0.0;
    double swing_low = 0.0;
    for (int i = pivot_left; i <= last - pivot_right; ++i) {
        bool high = true;
        bool low = true;
        for (int j = i - pivot_left; j <= i + pivot_right; ++j) {
            if (j == i)
                continue;
            high = high && bars[i].high > bars[j].high;
            low = low && bars[i].low < bars[j].low;
        }
        if (high)
            swing_high = bars[i].high;
        if (low)
            swing_low = bars[i].low;
    }
    out.confirmed_swing_high = swing_high;
    out.confirmed_swing_low = swing_low;
    out.signal_bar_ms = bars[last].bucket_ms;
    out.reference_price = bars[last].close;
    if (swing_high <= 0.0 || swing_low <= 0.0 || out.reference_price <= 0.0) {
        out.reason = QStringLiteral("NO_CONFIRMED_STRUCTURE");
        return out;
    }

    double tr_sum = 0.0;
    const int atr_start = std::max(1, static_cast<int>(bars.size()) - atr_period);
    int tr_count = 0;
    for (int i = atr_start; i < bars.size(); ++i) {
        const double previous = bars[i - 1].close;
        const double tr = std::max({bars[i].high - bars[i].low,
                                    std::abs(bars[i].high - previous),
                                    std::abs(bars[i].low - previous)});
        tr_sum += tr;
        ++tr_count;
    }
    const double atr = tr_count > 0 ? tr_sum / static_cast<double>(tr_count) : 0.0;
    out.atr_bps = atr / out.reference_price * 10000.0;

    const double fast = ema_close(bars, 4);
    const double slow = ema_close(bars, 9);
    const double previous_close = bars[last - 1].close;
    const bool broke_up = previous_close <= swing_high && bars[last].close > swing_high;
    const bool broke_down = previous_close >= swing_low && bars[last].close < swing_low;
    if (!broke_up && !broke_down) {
        out.reason = QStringLiteral("NO_CLOSED_BAR_BREAK");
        return out;
    }

    const double break_bps = broke_up
        ? (bars[last].close / swing_high - 1.0) * 10000.0
        : (swing_low / bars[last].close - 1.0) * 10000.0;
    const double trend_strength = out.atr_bps > 0.0
        ? std::min(1.0, std::abs(fast - slow) / atr)
        : 0.0;
    const double break_strength = out.atr_bps > 0.0
        ? std::min(1.0, break_bps / out.atr_bps)
        : 0.0;
    out.confidence = std::clamp(0.50 * trend_strength + 0.50 * break_strength, 0.0, 1.0);
    out.expected_move_bps = std::max(0.0, out.atr_bps * (0.50 + 0.50 * out.confidence));
    out.call = broke_up ? QStringLiteral("BUY") : QStringLiteral("SELL");

    // A break against the EMA regime is a change-of-character; a break in the
    // same direction is continuation structure. Both are objectively defined
    // from closed bars and neither uses future ticks.
    const bool prior_up_regime = fast >= slow;
    out.structure = (broke_up != prior_up_regime)
        ? QStringLiteral("CHOCH")
        : QStringLiteral("BOS");
    out.reason = QStringLiteral("CLOSED_BAR_%1").arg(out.structure);
    return out;
}

inline VenueProposal build_proposal(
    const ReversalSignal& signal,
    const crypto_latency::CryptoLatencyTick& quote,
    const VenueCost& cost,
    QString liquidity,
    qint64 now_ms,
    qint64 max_quote_age_ms,
    double min_net_edge_bps,
    qint64 ttl_ms) {
    VenueProposal out;
    out.venue = cost.venue;
    out.source = cost.source;
    out.side = signal.call.toLower();
    out.liquidity = liquidity = liquidity.trimmed().toLower();
    out.time_in_force = liquidity == QLatin1String("maker") ? QStringLiteral("GTC")
                                                             : QStringLiteral("IOC");
    out.bid = quote.best_bid;
    out.ask = quote.best_ask;
    out.quote_age_ms = quote.received_ts_ms > 0 ? std::max<qint64>(0, now_ms - quote.received_ts_ms) : -1;
    out.ttl_ms = ttl_ms;
    if (out.bid > 0.0 && out.ask > out.bid)
        out.spread_bps = (out.ask - out.bid) / ((out.ask + out.bid) * 0.5) * 10000.0;
    out.entry_fee_bps = liquidity == QLatin1String("maker") ? cost.maker_bps : cost.taker_bps;
    // A fast exit is always budgeted as taker. This prevents a hypothetical
    // all-maker round trip from manufacturing edge that cannot be captured.
    out.exit_fee_bps = cost.taker_bps;
    out.estimated_slippage_bps = liquidity == QLatin1String("maker") ? 0.0 : cost.slippage_bps;
    const double crossing = liquidity == QLatin1String("maker") ? 0.0 : out.spread_bps;
    out.round_trip_cost_bps = out.entry_fee_bps + out.exit_fee_bps +
                              (2.0 * out.estimated_slippage_bps) + crossing + cost.safety_bps;
    out.expected_move_bps = signal.expected_move_bps;
    out.net_edge_bps = out.expected_move_bps - out.round_trip_cost_bps;
    if (signal.call != QLatin1String("BUY") && signal.call != QLatin1String("SELL"))
        out.blockers << QStringLiteral("NO_SIGNAL");
    if (out.quote_age_ms < 0 || out.quote_age_ms > max_quote_age_ms)
        out.blockers << QStringLiteral("STALE_QUOTE");
    if (out.bid <= 0.0 || out.ask <= 0.0 || out.ask < out.bid)
        out.blockers << QStringLiteral("INVALID_BOOK");
    if (out.net_edge_bps < min_net_edge_bps)
        out.blockers << QStringLiteral("COST_NET_EDGE_BELOW_GATE");
    if (ttl_ms < 100 || ttl_ms > 5000)
        out.blockers << QStringLiteral("INVALID_TTL");
    out.limit_price = signal.call == QLatin1String("BUY")
        ? (liquidity == QLatin1String("maker") ? out.bid : out.ask)
        : (liquidity == QLatin1String("maker") ? out.ask : out.bid);
    out.executable = out.blockers.isEmpty();
    return out;
}

inline int choose_best_proposal(const QVector<VenueProposal>& proposals) {
    int best = -1;
    for (int i = 0; i < proposals.size(); ++i) {
        if (!proposals[i].executable)
            continue;
        if (best < 0 || proposals[i].net_edge_bps > proposals[best].net_edge_bps)
            best = i;
    }
    return best;
}

inline QJsonObject signal_json(const ReversalSignal& s) {
    return QJsonObject{{"call", s.call}, {"structure", s.structure},
                       {"signal_bar_ms", QString::number(s.signal_bar_ms)},
                       {"reference_price", s.reference_price}, {"atr_bps", s.atr_bps},
                       {"expected_move_bps", s.expected_move_bps},
                       {"confidence", s.confidence},
                       {"confirmed_swing_high", s.confirmed_swing_high},
                       {"confirmed_swing_low", s.confirmed_swing_low},
                       {"reason", s.reason},
                       {"non_repainting", true}, {"uses_closed_bars_only", true}};
}

inline QJsonObject proposal_json(const VenueProposal& p) {
    return QJsonObject{{"venue", p.venue}, {"source", p.source}, {"side", p.side},
                       {"liquidity", p.liquidity}, {"time_in_force", p.time_in_force},
                       {"limit_price", p.limit_price}, {"bid", p.bid}, {"ask", p.ask},
                       {"spread_bps", p.spread_bps}, {"entry_fee_bps", p.entry_fee_bps},
                       {"exit_fee_bps", p.exit_fee_bps},
                       {"estimated_slippage_bps", p.estimated_slippage_bps},
                       {"round_trip_cost_bps", p.round_trip_cost_bps},
                       {"expected_move_bps", p.expected_move_bps},
                       {"net_edge_bps", p.net_edge_bps},
                       {"quote_age_ms", QString::number(p.quote_age_ms)},
                       {"ttl_ms", QString::number(p.ttl_ms)},
                       {"executable", p.executable},
                       {"blockers", QJsonArray::fromStringList(p.blockers)}};
}

inline QString opportunity_id(const QString& symbol, const ReversalSignal& signal) {
    const QByteArray material = symbol.toUtf8() + '\0' + signal.call.toUtf8() + '\0' +
                                QByteArray::number(signal.signal_bar_ms);
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(24));
}

} // namespace openmarketterminal::services::crypto_scalp
