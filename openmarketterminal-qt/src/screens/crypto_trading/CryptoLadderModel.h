#pragma once
// Crypto DOM Ladder — pure price-bucketing + depth row window model.
// No Qt widgets, no DB, no network: just Qt containers + math. Read-only.

#include <QHash>
#include <QPair>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::crypto {

struct LadderRow {
    double price = 0;
    double bid_size = 0;
    double ask_size = 0;
    double vap = 0;
    double my_bid_qty = 0;
    double my_ask_qty = 0;
    bool   is_avg_entry = false;
};

struct LadderView {
    QVector<LadderRow> rows; // top (highest price) -> bottom (lowest)
    double mid = 0;
    double max_depth = 0;
    double max_vap = 0;
};

struct MyOrder { double price = 0; double qty = 0; bool is_buy = false; };

/// Integer bucket index for `price` at increment `grouping` (grouping > 0).
/// Used as the sole hash key for accumulation and row lookup so both sides
/// use an identical integer key — no float last-bit divergence risk.
inline qint64 bucket_index(double price, double grouping) {
    if (grouping <= 0) return 0;
    return static_cast<qint64>(std::floor(price / grouping + 1e-9));
}

/// Snap a price down to its bucket at increment `grouping` (grouping > 0).
inline double bucket_price(double price, double grouping) {
    if (grouping <= 0) return price;
    return bucket_index(price, grouping) * grouping;
}

class CryptoLadderModel {
  public:
    // depth: raw {price,size} levels; grouping: display increment; rows_each_side:
    // rows above and below mid. VAP/orders/avg default-empty until Tasks 2-3.
    LadderView build(const QVector<QPair<double,double>>& bids,
                     const QVector<QPair<double,double>>& asks,
                     double grouping, int rows_each_side) const {
        LadderView v;
        if (grouping <= 0 || rows_each_side <= 0)
            return v;
        double best_bid = 0, best_ask = 0;
        for (const auto& b : bids) best_bid = std::max(best_bid, b.first);
        for (const auto& a : asks) best_ask = (best_ask == 0) ? a.first : std::min(best_ask, a.first);
        if (best_bid <= 0 || best_ask <= 0)
            return v;
        v.mid = (best_bid + best_ask) / 2.0;
        const qint64 mid_idx = bucket_index(v.mid, grouping);

        QHash<qint64,double> bid_by_bucket, ask_by_bucket;
        for (const auto& b : bids) bid_by_bucket[bucket_index(b.first, grouping)] += b.second;
        for (const auto& a : asks) ask_by_bucket[bucket_index(a.first, grouping)] += a.second;

        // rows_each_side above and below the mid bucket, plus the mid bucket
        for (int i = rows_each_side; i >= -rows_each_side; --i) {
            LadderRow r;
            const qint64 idx = mid_idx + i;
            r.price = idx * grouping;
            r.bid_size = bid_by_bucket.value(idx, 0.0);
            r.ask_size = ask_by_bucket.value(idx, 0.0);
            v.max_depth = std::max({v.max_depth, r.bid_size, r.ask_size});
            v.rows.append(r);
        }
        return v;
    }
};

} // namespace openmarketterminal::crypto
