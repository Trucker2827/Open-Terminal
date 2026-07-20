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

double top_book_imbalance(double bid_size, double ask_size) {
    const double total = bid_size + ask_size;
    if (bid_size <= 0.0 || ask_size <= 0.0 || total <= 0.0)
        return 0.0;
    return clamp_unit((bid_size - ask_size) / total);
}

double microprice(double bid, double ask, double bid_size, double ask_size) {
    if (bid <= 0.0 || ask <= 0.0 || ask < bid)
        return 0.0;
    const double total = bid_size + ask_size;
    if (bid_size <= 0.0 || ask_size <= 0.0 || total <= 0.0)
        return (bid + ask) / 2.0;
    // Larger bid depth shifts the executable-pressure estimate toward the ask,
    // while larger ask depth shifts it toward the bid.
    return ((ask * bid_size) + (bid * ask_size)) / total;
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
    int anchor_index = -1;
    for (int i = 0; i < ticks_.size(); ++i) {
        if (ticks_[i].received_ts_ms <= cutoff)
            anchor_index = i;
        else
            break;
    }
    if (anchor_index >= 0)
        rows.push_back(ticks_[anchor_index]);
    for (int i = anchor_index + 1; i < ticks_.size(); ++i)
        rows.push_back(ticks_[i]);
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
    for (const auto& row : rows) {
        // rows may include one pre-window anchor for price-move continuity;
        // never count that older observation as executed volume in this window.
        if (!row.is_trade || row.received_ts_ms < cutoff) continue;
        const double size = row.trade_size > 0.0 ? row.trade_size : 1.0;
        if (row.aggressor_side == QLatin1String("buy")) {
            out.aggressor_buy_volume += size;
            ++out.classified_trades;
        } else if (row.aggressor_side == QLatin1String("sell")) {
            out.aggressor_sell_volume += size;
            ++out.classified_trades;
        } else {
            out.unclassified_trade_volume += size;
            ++out.unclassified_trades;
        }
    }
    const double classified_volume = out.aggressor_buy_volume + out.aggressor_sell_volume;
    const double total_trade_volume = classified_volume + out.unclassified_trade_volume;
    out.aggressor_pressure = classified_volume > 0.0
        ? clamp_unit((out.aggressor_buy_volume - out.aggressor_sell_volume) / classified_volume)
        : 0.0;
    out.aggressor_coverage = total_trade_volume > 0.0
        ? std::clamp(classified_volume / total_trade_volume, 0.0, 1.0) : 0.0;
    const int directional = out.upticks + out.downticks;
    out.tape_pressure = directional > 0
                            ? clamp_unit(static_cast<double>(out.upticks - out.downticks) /
                                         static_cast<double>(directional))
                            : 0.0;
    out.move_pct = ((out.end_price / out.start_price) - 1.0) * 100.0;
    out.coverage_ms = out.end_price > 0.0
                          ? rows.last().received_ts_ms - rows.first().received_ts_ms
                          : 0;
    // A named 15s or 60s signal must contain that amount of history. A short
    // one-shot capture is useful for warm-up, but it must not masquerade as a
    // completed longer-horizon measurement.
    out.available = out.coverage_ms >= static_cast<qint64>(seconds) * 1000;
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
        auto quote = tick;
        if (quote.best_bid <= 0.0 || quote.best_ask <= 0.0 ||
            quote.bid_size <= 0.0 || quote.ask_size <= 0.0) {
            // Trade ticks normally arrive more frequently than quote updates.
            // Keep the last fresh quote rather than mistaking a trade-only tick
            // for an empty order book.
            for (auto it = ticks_.crbegin(); it != ticks_.crend(); ++it) {
                if (it->source == state.source && it->best_bid > 0.0 && it->best_ask > 0.0 &&
                    it->bid_size > 0.0 && it->ask_size > 0.0) {
                    quote = *it;
                    break;
                }
            }
        }
        CryptoMicrostructureSource row;
        row.source = state.source;
        row.status = state.status;
        row.message = state.error.isEmpty() ? state.last_message_type : state.error;
        row.price = tick.price;
        row.best_bid = quote.best_bid;
        row.best_ask = quote.best_ask;
        row.bid_size = quote.bid_size;
        row.ask_size = quote.ask_size;
        row.microprice = microprice(quote.best_bid, quote.best_ask, quote.bid_size, quote.ask_size);
        row.top_book_imbalance = top_book_imbalance(quote.bid_size, quote.ask_size);
        row.spread_bps = spread_bps(quote.best_bid, quote.best_ask);
        row.age_ms = state.last_tick_ms > 0 ? now - state.last_tick_ms : -1;
        row.quote_age_ms = quote.received_ts_ms > 0 ? now - quote.received_ts_ms : -1;
        row.ticks = state.ticks;
        out.sources.push_back(row);

        if (row.quote_age_ms >= 0 && row.quote_age_ms <= 3000 &&
            quote.best_bid > 0.0 && quote.best_ask > 0.0 &&
            quote.bid_size > 0.0 && quote.ask_size > 0.0) {
            book_sum += row.top_book_imbalance;
            book_weight += 1.0;
            ++out.top_book_sources;
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
    double microprice_sum = 0.0;
    int microprice_sources = 0;
    for (const auto& row : out.sources) {
        if (row.microprice > 0.0 && row.quote_age_ms >= 0 && row.quote_age_ms <= 3000) {
            microprice_sum += row.microprice;
            ++microprice_sources;
        }
    }
    out.microprice = microprice_sources > 0
                         ? microprice_sum / static_cast<double>(microprice_sources)
                         : out.reference_price;

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

    if (primary.available) {
        out.aggressor_pressure = primary.aggressor_pressure;
        out.aggressor_coverage = primary.aggressor_coverage;
        out.aggressor_buy_volume = primary.aggressor_buy_volume;
        out.aggressor_sell_volume = primary.aggressor_sell_volume;
        out.classified_trades = primary.classified_trades;
    }

    const bool aggressor_ready = out.classified_trades >= 5 && out.aggressor_coverage >= 0.60;
    const double combined = aggressor_ready
        ? out.aggressor_pressure * 0.45 + out.tape_pressure * 0.35 + out.book_pressure * 0.20
        : out.tape_pressure * 0.70 + out.book_pressure * 0.30;
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
        out.rationale = QStringLiteral("tick-direction and top-book pressure are not directional");
    } else {
        out.call = out.confidence >= 0.35 ? QStringLiteral("TRADE CANDIDATE")
                                          : QStringLiteral("WATCH");
        out.rationale = QStringLiteral("tick=%1 top_book=%2 spread=%3bps")
                            .arg(out.tape_pressure, 0, 'f', 2)
                            .arg(out.book_pressure, 0, 'f', 2)
                            .arg(out.cross_source_spread_bps, 0, 'f', 2);
    }
    out.aggressive_trade_flow_status = aggressor_ready
        ? QStringLiteral("available: classified buyer/seller initiated volume")
        : QStringLiteral("warming: insufficient classified trade volume");
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
                                   {"bid_size", s.bid_size},
                                   {"ask_size", s.ask_size},
                                   {"microprice", s.microprice},
                                   {"top_book_imbalance", s.top_book_imbalance},
                                   {"spread_bps", s.spread_bps},
                                   {"age_ms", QString::number(s.age_ms)},
                                   {"quote_age_ms", QString::number(s.quote_age_ms)},
                                   {"ticks", s.ticks}});
    }
    QJsonArray windows;
    for (const auto& w : snapshot.windows) {
        windows.append(QJsonObject{{"seconds", w.seconds},
                                   {"coverage_ms", QString::number(w.coverage_ms)},
                                   {"upticks", w.upticks},
                                   {"downticks", w.downticks},
                                   {"flat_ticks", w.flat_ticks},
                                   {"start_price", w.start_price},
                                   {"end_price", w.end_price},
                                   {"move_pct", w.move_pct},
                                   {"tape_pressure", w.tape_pressure},
                                   {"aggressor_buy_volume", w.aggressor_buy_volume},
                                   {"aggressor_sell_volume", w.aggressor_sell_volume},
                                   {"unclassified_trade_volume", w.unclassified_trade_volume},
                                   {"aggressor_pressure", w.aggressor_pressure},
                                   {"aggressor_coverage", w.aggressor_coverage},
                                   {"classified_trades", w.classified_trades},
                                   {"unclassified_trades", w.unclassified_trades},
                                   {"available", w.available}});
    }
    return QJsonObject{{"schema_version", 1},
                       {"event", "crypto_microstructure_snapshot"},
                       {"observed_at_ms", QString::number(QDateTime::currentMSecsSinceEpoch())},
                       {"symbol", snapshot.symbol},
                       {"call", snapshot.call},
                       {"direction", snapshot.direction},
                       {"rationale", snapshot.rationale},
                       {"freshest_source", snapshot.freshest_source},
                       {"freshest_age_ms", QString::number(snapshot.freshest_age_ms)},
                       {"reference_price", snapshot.reference_price},
                       {"microprice", snapshot.microprice},
                       {"cross_source_spread_bps", snapshot.cross_source_spread_bps},
                       {"book_pressure", snapshot.book_pressure},
                       {"tape_pressure", snapshot.tape_pressure},
                       {"aggressor_pressure", snapshot.aggressor_pressure},
                       {"aggressor_coverage", snapshot.aggressor_coverage},
                       {"aggressor_buy_volume", snapshot.aggressor_buy_volume},
                       {"aggressor_sell_volume", snapshot.aggressor_sell_volume},
                       {"classified_trades", snapshot.classified_trades},
                       {"confidence", snapshot.confidence},
                       {"live_sources", snapshot.live_sources},
                       {"top_book_sources", snapshot.top_book_sources},
                       {"tick_count", snapshot.tick_count},
                       {"tick_pressure_kind", snapshot.tick_pressure_kind},
                       {"aggressive_trade_flow_status", snapshot.aggressive_trade_flow_status},
                       {"sources", sources},
                       {"windows", windows}};
}

} // namespace openmarketterminal::services::edge_radar
