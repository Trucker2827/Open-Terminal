#pragma once
// MakerQuotes.h — pure resting-price geometry for the maker spread producer.
// A market maker rests a bid below mid and an ask above mid; if price trades
// THROUGH a quote (try_maker_fill), the maker is filled at that adverse price.
// This is the only honest math in the producer worth isolating; everything
// else (symbol/venue/freshness/ts) is engine plumbing.
#include <QString>

namespace openmarketterminal::services::sandbox {

struct MakerQuote {
    QString side;        // "buy" (bid) | "sell" (ask)
    double limit_price = 0.0;
};

struct MakerQuotePair {
    bool valid = false;  // false iff mid was non-positive (nothing to quote)
    MakerQuote bid;
    MakerQuote ask;
};

// bid rests at mid*(1 - half_spread_bps/1e4); ask at mid*(1 + half_spread_bps/1e4).
MakerQuotePair build_maker_quotes(double mid, double half_spread_bps);

// Appends a bid+ask decision row pair to the maker_decisions journal at `path`
// (created if absent). No-op when mid is non-positive. reference_price carries
// the resting quote price (the half-spread is already applied).
void append_maker_decisions(const QString& path, const QString& symbol, const QString& venue,
                            double mid, double half_spread_bps, double freshest_age_ms,
                            int live_sources, qint64 ts_ms);

} // namespace openmarketterminal::services::sandbox
