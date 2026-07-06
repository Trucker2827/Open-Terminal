#include "services/edge_radar/CryptoMicrostructureRadar.h"

#include <QDateTime>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::edge_radar {

namespace latency = openmarketterminal::services::crypto_latency;

namespace {

double clamp_unit(double v) {
    if (!std::isfinite(v))
        return 0.0;
    return std::clamp(v, -1.0, 1.0);
}

double spread_bps(double bid, double ask) {
    if (bid <= 0.0 || ask <= 0.0 || ask < bid)
        return 0.0;
    const double mid = (bid + ask) / 2.0;
    return mid > 0.0 ? ((ask - bid) / mid) * 10000.0 : 0.0;
}

int source_status_rank(const CryptoMicrostructureSource& row) {
    if (row.status == QStringLiteral("live"))
        return 0;
    if (row.status == QStringLiteral("connected"))
        return 1;
    if (row.status == QStringLiteral("connecting"))
        return 2;
    return 3;
}

} // namespace

void CryptoMicrostructureRadar::clear() {
    ticks_.clear();
}

void CryptoMicrostructureRadar::add_tick(const latency::CryptoLatencyTick& tick) {
    if (tick.price <= 0.0 || tick.received_ts_ms <= 0)
        return;
    ticks_.push_back(tick);
    std::stable_sort(ticks_.begin(), ticks_.end(), [](const auto& a, const auto& b) {
        if (a.received_ts_ms != b.received_ts_ms)
            return a.received_ts_ms < b.received_ts_ms;
        return a.sequence < b.sequence;
    });
    const qint64 newest = ticks_.isEmpty() ? 0 : ticks_.last().received_ts_ms;
    while (!ticks_.isEmpty() && newest - ticks_.first().received_ts_ms > 180000)
        ticks_.removeFirst();
}

CryptoMicrostructureWindow CryptoMicrostructureRadar::window(int seconds) const {
    CryptoMicrostructureWindow out;
    out.seconds = seconds;
    if (seconds <= 0 || ticks_.size() < 2)
        return out;

    const auto& end = ticks_.last();
    const qint64 cutoff = end.received_ts_ms - static_cast<qint64>(seconds) * 1000;
    QVector<latency::CryptoLatencyTick> rows;
    for (const auto& tick : ticks_) {
        if (tick.received_ts_ms >= cutoff)
            rows.push_back(tick);
    }
    if (rows.size() < 2)
        return out;

    out.start_price = rows.first().price;
    out.end_price = rows.last().price;
    if (out.start_price <= 0.0 || out.end_price <= 0.0)
        return out;
    for (int i = 1; i < rows.size(); ++i) {
        if (rows[i].price > rows[i - 1].price)
            ++out.upticks;
        else if (rows[i].price < rows[i - 1].price)
            ++out.downticks;
        else
            ++out.flat_ticks;
    }
    const int directional = out.upticks + out.downticks;
    out.tape_pressure = directional > 0
                            ? clamp_unit(static_cast<double>(out.upticks - out.downticks) /
                                         static_cast<double>(directional))
                            : 0.0;
    out.move_pct = ((out.end_price / out.start_price) - 1.0) * 100.0;
    out.available = true;
    return out;
}

CryptoMicrostructureSnapshot CryptoMicrostructureRadar::snapshot(
    const latency::CryptoLatencySnapshot& latency_snapshot) const {
    CryptoMicrostructureSnapshot out;
    out.symbol = latency_snapshot.symbol;
    out.freshest_source = latency_snapshot.freshest_source;
    out.freshest_age_ms = latency_snapshot.freshest_age_ms;
    out.reference_price = latency_snapshot.mid_price;
    out.cross_source_spread_bps = latency_snapshot.cross_source_spread_bps;
    out.live_sources = latency_snapshot.live_sources;
    out.tick_count = ticks_.size();
    out.windows = {window(5), window(15), window(60)};

    QHash<QString, latency::CryptoLatencyTick> latest;
    for (const auto& tick : latency_snapshot.latest_ticks)
        latest.insert(tick.source, tick);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double book_sum = 0.0;
    double book_weight = 0.0;
    for (const auto& state : latency_snapshot.sources) {
        const auto tick = latest.value(state.source);
        CryptoMicrostructureSource row;
        row.source = state.source;
        row.status = state.status;
        row.message = state.error.isEmpty() ? state.last_message_type : state.error;
        row.price = tick.price;
        row.best_bid = tick.best_bid;
        row.best_ask = tick.best_ask;
        row.spread_bps = spread_bps(tick.best_bid, tick.best_ask);
        row.age_ms = state.last_tick_ms > 0 ? now - state.last_tick_ms : -1;
        row.ticks = state.ticks;
        out.sources.push_back(row);

        if (tick.best_bid > 0.0 && tick.best_ask > 0.0) {
            const double mid = (tick.best_bid + tick.best_ask) / 2.0;
            const double price_vs_mid = mid > 0.0 ? (tick.price - mid) / mid : 0.0;
            book_sum += std::clamp(price_vs_mid * 10000.0, -1.0, 1.0);
            book_weight += 1.0;
        }
    }
    std::stable_sort(out.sources.begin(), out.sources.end(), [](const auto& a, const auto& b) {
        const int ar = source_status_rank(a);
        const int br = source_status_rank(b);
        if (ar != br)
            return ar < br;
        if (ar == 0 && a.age_ms != b.age_ms) {
            if (a.age_ms < 0)
                return false;
            if (b.age_ms < 0)
                return true;
            return a.age_ms < b.age_ms;
        }
        if (a.ticks != b.ticks)
            return a.ticks > b.ticks;
        return a.source < b.source;
    });
    out.book_pressure = book_weight > 0.0 ? clamp_unit(book_sum / book_weight) : 0.0;

    CryptoMicrostructureWindow primary;
    for (const auto& w : out.windows) {
        if (w.available && w.seconds == 15) {
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
    if (primary.available)
        out.tape_pressure = primary.tape_pressure;

    const double combined = out.tape_pressure * 0.70 + out.book_pressure * 0.30;
    out.confidence = std::clamp(std::abs(combined), 0.0, 1.0);
    out.direction = combined > 0.12 ? QStringLiteral("up")
                   : combined < -0.12 ? QStringLiteral("down")
                                       : QStringLiteral("flat");
    if (out.freshest_age_ms < 0 || out.freshest_age_ms > 3000) {
        out.call = QStringLiteral("NO TRADE");
        out.rationale = QStringLiteral("freshest BTC tick is stale");
    } else if (out.live_sources < 2) {
        out.call = QStringLiteral("WATCH");
        out.rationale = QStringLiteral("need at least two live sources before trusting pressure");
    } else if (out.cross_source_spread_bps > 12.0) {
        out.call = QStringLiteral("WATCH");
        out.rationale = QStringLiteral("exchange prices disagree; possible lag/outlier");
    } else if (out.direction == QStringLiteral("flat")) {
        out.call = QStringLiteral("NO TRADE");
        out.rationale = QStringLiteral("tape pressure is not directional");
    } else {
        out.call = out.confidence >= 0.35 ? QStringLiteral("TRADE CANDIDATE")
                                          : QStringLiteral("WATCH");
        out.rationale = QStringLiteral("tape=%1 book=%2 spread=%3bps")
                            .arg(out.tape_pressure, 0, 'f', 2)
                            .arg(out.book_pressure, 0, 'f', 2)
                            .arg(out.cross_source_spread_bps, 0, 'f', 2);
    }
    return out;
}

QJsonObject CryptoMicrostructureRadar::to_json(const CryptoMicrostructureSnapshot& snapshot) {
    QJsonArray sources;
    for (const auto& s : snapshot.sources) {
        sources.append(QJsonObject{{"source", s.source},
                                   {"status", s.status},
                                   {"message", s.message},
                                   {"price", s.price},
                                   {"best_bid", s.best_bid},
                                   {"best_ask", s.best_ask},
                                   {"spread_bps", s.spread_bps},
                                   {"age_ms", QString::number(s.age_ms)},
                                   {"ticks", s.ticks}});
    }
    QJsonArray windows;
    for (const auto& w : snapshot.windows) {
        windows.append(QJsonObject{{"seconds", w.seconds},
                                   {"upticks", w.upticks},
                                   {"downticks", w.downticks},
                                   {"flat_ticks", w.flat_ticks},
                                   {"start_price", w.start_price},
                                   {"end_price", w.end_price},
                                   {"move_pct", w.move_pct},
                                   {"tape_pressure", w.tape_pressure},
                                   {"available", w.available}});
    }
    return QJsonObject{{"symbol", snapshot.symbol},
                       {"call", snapshot.call},
                       {"direction", snapshot.direction},
                       {"rationale", snapshot.rationale},
                       {"freshest_source", snapshot.freshest_source},
                       {"freshest_age_ms", QString::number(snapshot.freshest_age_ms)},
                       {"reference_price", snapshot.reference_price},
                       {"cross_source_spread_bps", snapshot.cross_source_spread_bps},
                       {"book_pressure", snapshot.book_pressure},
                       {"tape_pressure", snapshot.tape_pressure},
                       {"confidence", snapshot.confidence},
                       {"live_sources", snapshot.live_sources},
                       {"tick_count", snapshot.tick_count},
                       {"sources", sources},
                       {"windows", windows}};
}

} // namespace openmarketterminal::services::edge_radar
