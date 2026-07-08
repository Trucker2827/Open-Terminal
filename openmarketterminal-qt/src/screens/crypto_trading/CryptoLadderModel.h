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
    // fine resolution used to key the session VAP accumulator; independent
    // of the display grouping passed to build().
    explicit CryptoLadderModel(double vap_base = 0.1) : vap_base_(vap_base) {}

    // Accumulate traded volume at `price` into the fine-base bucket. Amounts
    // <= 0 are ignored (no-op trades / defensive against bad ticks).
    void accumulate_vap(double price, double amount) {
        if (amount <= 0) return;
        vap_[bucket_index(price, vap_base_)] += amount;
    }

    // Clear the session VAP accumulator (e.g. on session/day rollover).
    void reset_vap() { vap_.clear(); }

    // depth: raw {price,size} levels; grouping: display increment; rows_each_side:
    // rows above and below mid. VAP is re-aggregated from the fine-base
    // accumulator into the display grouping on every build().
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

        // Re-aggregate the fine-base VAP accumulator into display buckets:
        // fine index `fi` at vap_base_ maps to fine price fi*vap_base_, whose
        // display index is bucket_index(fi*vap_base_, grouping). Both sides
        // stay keyed by integer index, matching the depth keying above.
        QHash<qint64,double> vap_by_display;
        for (auto it = vap_.constBegin(); it != vap_.constEnd(); ++it) {
            const double fine_price = it.key() * vap_base_;
            const qint64 di = bucket_index(fine_price, grouping);
            vap_by_display[di] += it.value();
        }

        // rows_each_side above and below the mid bucket, plus the mid bucket
        for (int i = rows_each_side; i >= -rows_each_side; --i) {
            LadderRow r;
            const qint64 idx = mid_idx + i;
            r.price = idx * grouping;
            r.bid_size = bid_by_bucket.value(idx, 0.0);
            r.ask_size = ask_by_bucket.value(idx, 0.0);
            r.vap = vap_by_display.value(idx, 0.0);
            v.max_depth = std::max({v.max_depth, r.bid_size, r.ask_size});
            v.max_vap = std::max(v.max_vap, r.vap);
            v.rows.append(r);
        }
        return v;
    }

  private:
    double vap_base_ = 0.1;
    QHash<qint64,double> vap_; // fine-base bucket index -> summed volume
};

} // namespace openmarketterminal::crypto
