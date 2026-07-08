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

// One aggregated level of a single book side, for the side-by-side book view.
struct BookLevel {
    double price = 0; // bucket price (multiple of grouping)
    double size  = 0; // summed size in this bucket
    double cum   = 0; // running cumulative size from best -> this level
};

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
        return build(bids, asks, grouping, rows_each_side, {}, 0.0);
    }

    // Overload with resting-order + average-entry overlays. `my_orders` are
    // folded into their bucket's my_bid_qty (is_buy) / my_ask_qty; the row
    // whose bucket contains `avg_entry_price` gets is_avg_entry = true.
    // Overlay maps are keyed by the same INTEGER bucket index as depth/VAP
    // (no float bucket_price / epsilon compare) so lookup can't miss due to
    // float last-bit divergence.
    LadderView build(const QVector<QPair<double,double>>& bids,
                     const QVector<QPair<double,double>>& asks,
                     double grouping, int rows_each_side,
                     const QVector<MyOrder>& my_orders,
                     double avg_entry_price) const {
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

        // Resting-order overlay, keyed by integer bucket index.
        QHash<qint64,double> mybuy, mysell;
        for (const auto& o : my_orders) {
            const qint64 oi = bucket_index(o.price, grouping);
            (o.is_buy ? mybuy : mysell)[oi] += o.qty;
        }
        const bool have_avg = avg_entry_price > 0;
        const qint64 avg_idx = have_avg ? bucket_index(avg_entry_price, grouping) : 0;

        // rows_each_side above and below the mid bucket, plus the mid bucket
        for (int i = rows_each_side; i >= -rows_each_side; --i) {
            LadderRow r;
            const qint64 idx = mid_idx + i;
            r.price = idx * grouping;
            r.bid_size = bid_by_bucket.value(idx, 0.0);
            r.ask_size = ask_by_bucket.value(idx, 0.0);
            r.vap = vap_by_display.value(idx, 0.0);
            r.my_bid_qty = mybuy.value(idx, 0.0);
            r.my_ask_qty = mysell.value(idx, 0.0);
            r.is_avg_entry = have_avg && (idx == avg_idx);
            v.max_depth = std::max({v.max_depth, r.bid_size, r.ask_size});
            v.max_vap = std::max(v.max_vap, r.vap);
            v.rows.append(r);
        }
        return v;
    }

    // Side-by-side book view: aggregate one side's raw {price,size} levels into
    // `grouping` price buckets, return the best `count` buckets ordered
    // best-first (descending price for bids, ascending for asks) with a running
    // cumulative size. Pure; keyed by the same integer bucket index as build().
    QVector<BookLevel> book_side(const QVector<QPair<double,double>>& levels,
                                 double grouping, int count, bool is_bid) const {
        QVector<BookLevel> out;
        if (grouping <= 0 || count <= 0)
            return out;
        QHash<qint64,double> by_bucket;
        for (const auto& l : levels) {
            if (l.first <= 0 || l.second <= 0) continue;
            by_bucket[bucket_index(l.first, grouping)] += l.second;
        }
        QVector<qint64> keys;
        keys.reserve(by_bucket.size());
        for (auto it = by_bucket.constBegin(); it != by_bucket.constEnd(); ++it)
            keys.append(it.key());
        std::sort(keys.begin(), keys.end(),
                  [is_bid](qint64 a, qint64 b) { return is_bid ? a > b : a < b; });
        const int n = std::min<int>(count, keys.size());
        double cum = 0;
        for (int i = 0; i < n; ++i) {
            BookLevel bl;
            bl.price = keys[i] * grouping;
            bl.size  = by_bucket.value(keys[i], 0.0);
            cum += bl.size;
            bl.cum = cum;
            out.append(bl);
        }
        return out;
    }

  private:
    double vap_base_ = 0.1;
    QHash<qint64,double> vap_; // fine-base bucket index -> summed volume
};

} // namespace openmarketterminal::crypto
