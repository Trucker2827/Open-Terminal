#!/usr/bin/env python3
"""Kalshi maker quote-lag engine (read-only). Measures whether RESTING a quote to
capture the book-lag after a >=sigma BTC move is a real edge, with a fill model that
BRACKETS queue position between optimistic (front) and pessimistic (back) bounds
computed from real trade prints. Certifies an edge only under the pessimistic bound.
NO live execution, ever. See docs/design/2026-07-30-kalshi-maker-quote-lag-design.md.

    python3 scripts/research/maker_quote_lag.py
"""
import bisect, collections, json, math, os, sys
import forecast_history
import kalshi_edge_common as common
import q1_quote_lag as q1

SCHEMA_VERSION = 1


def maker_fill(ahead_size, hits):
    """Bracket the fill of a resting order against the taker volume that would
    execute it. `hits` = sorted [(ts_ms, size)] of trades through our price on the
    taking side, after we joined. Optimistic = first hit (front of queue).
    Pessimistic = the hit at which cumulative size first exceeds `ahead_size`
    (back of queue), else None. Non-fill is a real outcome, not a free win.
    """
    if not hits:
        return {"optimistic": None, "pessimistic": None}
    optimistic = hits[0][0]
    cumulative = 0.0
    pessimistic = None
    for ts_ms, size in hits:
        cumulative += size
        if cumulative > ahead_size:
            pessimistic = ts_ms
            break
    return {"optimistic": optimistic, "pessimistic": pessimistic}


# ── trade prints + the event -> rest -> exit sweep ─────────────────────────
TRADES_FILE = "kalshi-trade-events.jsonl"

# Pre-registered sweep parameters (design doc, "Sweep parameters"): rest at the
# touch or one cent inside it; exit as a taker at each of these horizons after
# the triggering event. Kept local to this module (distinct from q1's
# diagnostic HORIZONS_S, which also reports 120s/300s momentum, not a sweep
# exit) and from the design's literal {5, 15, 30, 60}s / {touch, +1 tick}.
REST_OFFSET_TICKS = (0, 1)
TICK_SIZE = 0.01
EXIT_HORIZONS_S = (5, 15, 30, 60)

# `q1.QuoteBook` (built from `q1.load_quote_series`) exposes only
# (ts, bid, ask, mid) — the top-of-book *size* fields (`yes_bid_size_fp`/
# `yes_ask_size_fp`) that a real `ahead_size` needs are dropped by that
# reader because `load_quote_series` only keeps what a mid needs. The raw
# rows in `kalshi-tickers.jsonl` DO carry the size fields (verified against
# real evidence — see `load_sizes` below), so a real `ahead_size` is sourced
# from a second, size-only reader over the same file rather than from
# `q1.QuoteBook`. `ahead_size_source` on every record says which case
# applied: `AHEAD_SIZE_SOURCE_TOP_OF_BOOK` when a real size row was found
# at/before `join_ts`, `AHEAD_SIZE_SOURCE_NO_SIZE_ROW` (falling back to 0.0 —
# front-of-queue then equals back-of-queue, a real simplification, not a
# hidden one) when none was, and `AHEAD_SIZE_SOURCE_EMPTY_LEVEL` for a
# rest-offset strictly inside the spread (see `simulate_event`: such a price
# is provably empty, by definition of "touch", independent of whether a
# size row was found).
AHEAD_SIZE_SOURCE_TOP_OF_BOOK = "top_of_book"
AHEAD_SIZE_SOURCE_NO_SIZE_ROW = "no_size_row"
AHEAD_SIZE_SOURCE_EMPTY_LEVEL = "empty_level_inside_spread"


def load_sizes():
    """market_ticker -> sorted [(ts_ms, yes_bid_size, yes_ask_size)].

    Top-of-book sizes from `kalshi-tickers.jsonl` (via `common.read_jsonl`,
    same source and retained-series prepend as `q1.load_quote_series`) —
    read separately because `q1.QuoteBook` drops `yes_bid_size_fp`/
    `yes_ask_size_fp`. A row lacking either size field is skipped and
    counted; unlike `load_quote_series` this does NOT require a valid
    0 < bid < ask < 1 mid, since `ahead_size` only needs the resting side's
    own top-of-book depth, not a two-sided price.
    """
    records, _inventory = common.read_jsonl(q1.TICKERS)
    series = collections.defaultdict(list)
    dropped = 0
    for record in records:
        ticker = record.get("market_ticker")
        ts_ms = record.get("ts_ms")
        if not ticker or ts_ms is None:
            dropped += 1
            continue
        try:
            ts_ms = int(ts_ms)
            yes_bid_size = float(record.get("yes_bid_size_fp"))
            yes_ask_size = float(record.get("yes_ask_size_fp"))
        except (TypeError, ValueError):
            dropped += 1
            continue
        series[ticker].append((ts_ms, yes_bid_size, yes_ask_size))
    for ticker in series:
        series[ticker].sort()
    return dict(series), dropped


class SizeBook:
    """Backward-looking lookup of top-of-book size, built from `load_sizes`."""

    def __init__(self, series):
        self.series = series
        self.times = {t: [row[0] for row in rows] for t, rows in series.items()}

    def at(self, ticker, ts_ms):
        """(ts, yes_bid_size, yes_ask_size) at/before `ts_ms`, or None.

        Deliberately backward-only, like `q1.QuoteBook.at`: a size row
        recorded after `ts_ms` must never leak into a decision made at
        `ts_ms`. Unlike `QuoteBook.at` there is no staleness cutoff — a
        stale size is still the best available proxy for queue depth,
        whereas a stale price is not usable as a price at all.
        """
        times = self.times.get(ticker)
        if not times:
            return None
        idx = bisect.bisect_right(times, ts_ms) - 1
        if idx < 0:
            return None
        return self.series[ticker][idx]


def load_trades():
    """market_ticker -> sorted [(ts_ms, yes_price, size, taker_sold_yes)].

    From `kalshi-trade-events.jsonl`. `taker_sold_yes` sign mapping — verified
    empirically against the recorded top-of-book quotes (`kalshi-tickers.jsonl`
    joined on ticker + nearest prior ts), NOT assumed from the field names:
    trades with `taker_outcome_side == "no"` print at a `yes_price_dollars`
    that matches the contemporaneous YES BID ~60% of the time (vs. the YES ASK
    ~7%) — i.e. that taker's own order, denominated in NO and crossing the NO
    ask ("buy NO"), is mechanically a SELL of YES against the YES bid (buy NO
    == sell YES at the complementary price: they are the same payoff, so the
    same underlying book match). Symmetrically, `taker_outcome_side == "yes"`
    trades sit at the YES ASK ~61% of the time (vs. the YES bid ~8%) — a BUY
    of YES. So `taker_sold_yes = (taker_outcome_side == "no")` — the OPPOSITE
    of the naive same-label reading of `taker_book_side`. See the Task 2
    report for the join-to-quotes counts this was measured from.
    """
    records, _inventory = common.read_jsonl(TRADES_FILE)
    series = collections.defaultdict(list)
    for record in records:
        ticker = record.get("market_ticker")
        ts_ms = record.get("ts_ms")
        if not ticker or ts_ms is None:
            continue
        try:
            ts_ms = int(ts_ms)
            yes_price = float(record.get("yes_price_dollars"))
            size = float(record.get("count_fp"))
        except (TypeError, ValueError):
            continue
        outcome_side = record.get("taker_outcome_side") or record.get("taker_side")
        if outcome_side not in ("yes", "no"):
            continue
        taker_sold_yes = (outcome_side == "no")
        series[ticker].append((ts_ms, yes_price, size, taker_sold_yes))
    for ticker in series:
        series[ticker].sort()
    return dict(series)


def hits_against(bid_side, rest_price, join_ts, trades_for_ticker):
    """Taker volume that would execute a resting BUY of `bid_side` at
    `rest_price`, strictly after `join_ts`. A "hit" is a taker SELLING
    `bid_side` at a price (in `bid_side` terms) <= `rest_price`. For a NO bid,
    the bid-side price is `1 - yes_price` and "taker sold NO" is the
    complement of `taker_sold_yes`. Returns sorted [(ts_ms, size)].
    """
    hits = []
    for ts_ms, yes_price, size, taker_sold_yes in trades_for_ticker:
        if ts_ms <= join_ts:
            continue
        if bid_side == "YES":
            taker_sold_side = taker_sold_yes
            price_in_side_terms = yes_price
        else:
            taker_sold_side = not taker_sold_yes
            price_in_side_terms = 1.0 - yes_price
        if not taker_sold_side:
            continue
        if price_in_side_terms <= rest_price + 1e-9:
            hits.append((ts_ms, size))
    return hits


def simulate_event(event, book, trades, outcomes, sizes=None):
    """Rest a maker quote on the lagging side of `event`, fill it from real
    trade prints under both bounds, and exit as a taker at each horizon.

    `sizes` is a `SizeBook` (or None) supplying `ahead_size`: the RESTING
    side's top-of-book size at `join_ts` (the event ts `t`) — betting YES
    means we post a YES bid, so ahead ~= `yes_bid_size`; betting NO means we
    post a NO bid, which is the YES ASK side of the same book, so ahead ~=
    `yes_ask_size`. When `sizes` is None or has no row at/before `join_ts`,
    `ahead_size` falls back to 0.0 and `ahead_size_source` says so
    truthfully (`AHEAD_SIZE_SOURCE_NO_SIZE_ROW`) rather than pretending queue
    position is known.

    Returns one record per (ticker, rest-offset, horizon), each carrying
    `pnl_optimistic`, `pnl_pessimistic` (None when that bound never filled
    within the horizon — non-fill is a real outcome, not a free win) and
    `pnl_taker` (the same capture entered by crossing at the touch, as the
    taker baseline the maker version must beat). `won` is relative to the
    side rested here (True if OUR side settled), not the raw market outcome.
    """
    t = event["ts_ms"]
    side = "YES" if event["move_bps"] > 0 else "NO"
    records = []
    for ticker in book.series:
        parsed = common.parse_ticker(ticker)
        if parsed is None or parsed["strike"] is None:
            continue  # unparseable / band market: not a monotone threshold
        seconds_to_close = (parsed["close_ms"] - t) / 1000.0
        if not (q1.MIN_SECONDS_TO_CLOSE <= seconds_to_close <= q1.MAX_SECONDS_TO_CLOSE):
            continue
        quote = book.at(ticker, t)
        if quote is None:
            continue
        _, yes_bid, yes_ask, _mid = quote
        if side == "YES":
            touch_bid, touch_ask = yes_bid, yes_ask
        else:
            touch_bid, touch_ask = 1.0 - yes_ask, 1.0 - yes_bid
        ticker_trades = trades.get(ticker, [])
        market_won_yes = outcomes.get(ticker)
        won = None if market_won_yes is None else (
            market_won_yes if side == "YES" else not market_won_yes)

        touch_size_row = sizes.at(ticker, t) if sizes is not None else None
        if touch_size_row is None:
            touch_ahead_size = 0.0
            touch_ahead_size_source = AHEAD_SIZE_SOURCE_NO_SIZE_ROW
        else:
            _, yes_bid_size, yes_ask_size = touch_size_row
            # RESTING side at join: a YES bet posts a YES bid (ahead ==
            # yes_bid_size); a NO bet posts a NO bid, which sits on the YES
            # ASK side of the same book (ahead == yes_ask_size).
            touch_ahead_size = yes_bid_size if side == "YES" else yes_ask_size
            touch_ahead_size_source = AHEAD_SIZE_SOURCE_TOP_OF_BOOK

        for offset_ticks in REST_OFFSET_TICKS:
            rest_price = round(touch_bid + offset_ticks * TICK_SIZE, 4)
            if rest_price >= touch_ask:
                continue  # would cross the spread -- not a restable maker price
            if offset_ticks == 0:
                # Resting AT the touch: ahead of us is whatever top-of-book
                # size the feed reports on the resting side.
                ahead_size = touch_ahead_size
                ahead_size_source = touch_ahead_size_source
            else:
                # Resting strictly INSIDE the spread (price-improving): by
                # definition of "touch", nobody is quoting at this price yet
                # -- if they were, this price would itself be the touch. The
                # level is therefore provably empty, independent of whether
                # a size row was found; using the touch's size here would
                # overstate the queue ahead of us.
                ahead_size = 0.0
                ahead_size_source = AHEAD_SIZE_SOURCE_EMPTY_LEVEL
            hits = hits_against(side, rest_price, t, ticker_trades)
            fill = maker_fill(ahead_size, hits)

            for horizon in EXIT_HORIZONS_S:
                exit_ts = t + horizon * 1000
                exit_quote = book.at(ticker, exit_ts)
                if exit_quote is None:
                    continue
                _, exit_yes_bid, exit_yes_ask, _exit_mid = exit_quote
                exit_price = exit_yes_bid if side == "YES" else 1.0 - exit_yes_ask
                exit_fee = common.fee_per_contract(exit_price)
                entry_fee = common.fee_per_contract(rest_price)

                pnls = {}
                fills = {}
                for bound in ("optimistic", "pessimistic"):
                    ts_fill = fill[bound]
                    filled = ts_fill is not None and ts_fill <= exit_ts
                    fills[bound] = filled
                    pnls[bound] = ((exit_price - rest_price) - entry_fee - exit_fee
                                   if filled else None)

                taker_entry_fee = common.fee_per_contract(touch_ask)
                pnl_taker = ((exit_price - touch_ask) - taker_entry_fee - exit_fee)

                records.append({
                    "ticker": ticker,
                    "side": side,
                    "rest_offset_ticks": offset_ticks,
                    "rest_price": rest_price,
                    "horizon_s": horizon,
                    "ahead_size": ahead_size,
                    "ahead_size_source": ahead_size_source,
                    "pnl_optimistic": pnls["optimistic"],
                    "pnl_pessimistic": pnls["pessimistic"],
                    "pnl_taker": pnl_taker,
                    "filled_opt": fills["optimistic"],
                    "filled_pess": fills["pessimistic"],
                    "won": won,
                    "cluster": t,
                    "event_ts": common.iso(t),
                })
    return records


# ── statistics (strategy_grid.py is NOT on this branch -- these are
# re-implemented here, stdlib-only, mirroring its clustered_mean /
# benjamini_hochberg / walk-forward semantics so this module stands alone) ──

def clustered_mean(values, clusters):
    """Cluster-robust mean of `values`, clustered on the parallel `clusters`
    list (here: the triggering event's ts -- events sharing one BTC move are
    not independent draws, the autopsy's core lesson). `effective_n` is the
    design effect n^2 / sum(cluster_size^2): equals `n` when every cluster has
    size 1, shrinks toward the number of clusters as they grow.
    """
    n = len(values)
    if n == 0:
        return {"n": 0, "effective_n": 0.0, "mean": None, "se": None, "t": None,
                "ci95": [None, None]}
    mean = sum(values) / n
    sizes = collections.Counter(clusters)
    eff = (n * n) / sum(s * s for s in sizes.values())
    groups = collections.defaultdict(float)
    for v, c in zip(values, clusters):
        groups[c] += (v - mean)
    cr_var = sum(g * g for g in groups.values()) / (n * n)
    se = math.sqrt(cr_var)
    t = mean / se if se > 0 else None
    half = 1.96 * se
    return {"n": n, "effective_n": eff, "mean": mean, "se": se, "t": t,
            "ci95": [mean - half, mean + half]}


def _normal_sf(z):
    return 0.5 * math.erfc(z / math.sqrt(2.0))


def _two_sided_p(t):
    return 2.0 * _normal_sf(abs(t)) if t is not None else 1.0


def benjamini_hochberg(pvals, alpha=0.05):
    """Standard step-up BH: reject the largest rank k with p[k] <= alpha*k/m,
    and every rank below it (never a rank above, even if it independently
    clears its own threshold -- the step-up procedure is monotone in rank).
    """
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    rejected = [False] * m
    max_k = -1
    for rank, idx in enumerate(order, start=1):
        if pvals[idx] <= alpha * rank / m:
            max_k = rank
    for rank, idx in enumerate(order, start=1):
        if rank <= max_k:
            rejected[idx] = True
    return rejected


def _fold_bounds(sorted_ts, folds=5):
    m = len(sorted_ts)
    return [sorted_ts[min(m - 1, (m * k) // folds)] for k in range(1, folds + 1)]


def walkforward_holdout_mean(pairs, folds=5):
    """Final-holdout walk-forward check: the mean of `value` over the last
    ~1/folds (20% by default) of `pairs` ordered by `ts` (event time) -- the
    same fold boundary `strategy_grid.walkforward_delta` used, adapted from a
    delta-vs-null to a plain mean since certification here compares this
    holdout mean's SIGN against the full-sample mean, not its magnitude.
    `pairs` is [(ts, value), ...]; ts is the record's `cluster` (triggering
    event ts_ms), so a holdout fold is a block of EVENTS, not individual
    records -- consistent with `clustered_mean`'s clustering unit. Returns
    None when there is no data, or none falls after the fold boundary.
    """
    if not pairs:
        return None
    ordered = sorted(pairs, key=lambda pair: pair[0])
    ts_sorted = [ts for ts, _ in ordered]
    if len(ts_sorted) >= folds:
        bounds = _fold_bounds(ts_sorted, folds)
        last_train_close = bounds[-2]
    else:
        last_train_close = ts_sorted[0]
    holdout = [v for ts, v in ordered if ts > last_train_close]
    if not holdout:
        return None
    return sum(holdout) / len(holdout)


def _sign(x):
    return (x > 0) - (x < 0)


CERTIFY_MIN_EFFECTIVE_N = 30
BH_ALPHA = 0.05


def certify(cell):
    """A cell is certified iff it is positive under the PESSIMISTIC fill
    bound, survives Benjamini-Hochberg, keeps the same sign in the
    final-holdout walk-forward, and has adequate clustered effective sample.

    Certification reads ONLY `mean_pnl_pessimistic` / `walkforward_pessimistic`
    / `effective_n` / `bh_rejected` off `cell`. `mean_pnl_optimistic` and both
    fill rates are reported on the cell for context and are never consulted
    here -- an edge that clears only the optimistic bound is a flagged
    candidate, never certified (design doc, "Certification rule").
    """
    mean_pess = cell.get("mean_pnl_pessimistic")
    wf_pess = cell.get("walkforward_pessimistic")
    eff_n = cell.get("effective_n") or 0.0
    if mean_pess is None or wf_pess is None:
        return False
    if not cell.get("bh_rejected"):
        return False
    if eff_n < CERTIFY_MIN_EFFECTIVE_N:
        return False
    if not (mean_pess > 0.0):
        return False
    return _sign(wf_pess) == _sign(mean_pess)


# ── evidence artifact: run() + main() ───────────────────────────────────────

def _outcome_for(ticker, parsed, settlements, brti):
    """(outcome, source) -- recorded settlement first, derived fallback for
    KXBTCD only (mirrors `strategy_grid._outcome_for`). Only feeds the
    informational `won` field on each simulated record; pnl itself is marked
    to the quote at each horizon, never to settlement.
    """
    row = settlements.get(ticker)
    recorded = {"yes": True, "no": False}.get((row or {}).get("result")) if row else None
    if recorded is not None:
        return recorded, "recorded"
    if parsed["family"] == "KXBTCD":
        derived = common.derive_outcome(parsed, brti)
        if derived is not None:
            return derived, "derived"
    return None, None


def run(evidence):
    """Every retained sigma-event, swept through `simulate_event`, grouped
    into cells keyed by (threshold_sigma, side, rest_offset_ticks, horizon_s),
    scored under both fill bounds, corrected for multiple comparisons and
    certified under the pessimistic bound only. Read-only; `evidence` is
    accepted for interface symmetry with the other q*/f* scripts -- every
    loader below resolves its own path through `kalshi_edge_common`, which
    already honours `OPENTERMINAL_EVIDENCE_DIR`.
    """
    brti, brti_inventory = common.load_brti()
    quotes, quote_inventory, quote_dropped = q1.load_quote_series()
    trades = load_trades()
    sizes, sizes_dropped = load_sizes()
    book = q1.QuoteBook(quotes)
    sizebook = SizeBook(sizes)

    settlements, settlement_inventory = common.load_settlements(interesting=set(quotes))
    outcomes = {}
    outcome_source = collections.Counter()
    unresolved = 0
    for ticker in quotes:
        parsed = common.parse_ticker(ticker)
        if parsed is None:
            unresolved += 1
            continue
        outcome, source = _outcome_for(ticker, parsed, settlements, brti)
        if outcome is None:
            unresolved += 1
            continue
        outcomes[ticker] = outcome
        outcome_source[source] += 1

    quote_span = book.span()
    brti_span = brti.span_ms
    start = max(quote_span[0], brti_span[0]) if quote_span[0] and brti_span[0] else None
    end = min(quote_span[1], brti_span[1]) if quote_span[1] and brti_span[1] else None
    paired_hours = (end - start) / 3_600_000.0 if start is not None and end is not None else 0.0

    events_by_threshold = {}
    grouped = collections.defaultdict(list)
    if start is not None and end is not None and end > start:
        volatility = forecast_history.VolatilityGrid(brti)
        for threshold in q1.SIGMA_THRESHOLDS:
            events = q1.detect_events(brti, volatility, start, end, threshold)
            events_by_threshold[threshold] = len(events)
            for event in events:
                for record in simulate_event(event, book, trades, outcomes, sizes=sizebook):
                    record["threshold_sigma"] = threshold
                    key = (threshold, record["side"], record["rest_offset_ticks"],
                          record["horizon_s"])
                    grouped[key].append(record)

    cells = []
    pvals = []
    for key in sorted(grouped):
        threshold, side, offset_ticks, horizon = key
        records = grouped[key]
        n = len(records)
        n_opt_fills = sum(1 for r in records if r["filled_opt"])
        n_pess_fills = sum(1 for r in records if r["filled_pess"])
        opt_values = [r["pnl_optimistic"] for r in records if r["pnl_optimistic"] is not None]
        pess_pairs = [(r["cluster"], r["pnl_pessimistic"]) for r in records
                      if r["pnl_pessimistic"] is not None]
        pess_values = [v for _, v in pess_pairs]
        pess_clusters = [c for c, _ in pess_pairs]
        clustered = clustered_mean(pess_values, pess_clusters)
        taker_values = [r["pnl_taker"] for r in records]

        cell = {
            "threshold_sigma": threshold,
            "side": side,
            "rest_offset_ticks": offset_ticks,
            "horizon_s": horizon,
            "n": n,
            "n_events": len({r["cluster"] for r in records}),
            "n_filled_optimistic": n_opt_fills,
            "n_filled_pessimistic": n_pess_fills,
            "fill_rate_optimistic": (n_opt_fills / n) if n else None,
            "fill_rate_pessimistic": (n_pess_fills / n) if n else None,
            "mean_pnl_optimistic": (sum(opt_values) / len(opt_values)
                                    if opt_values else None),
            "mean_pnl_pessimistic": clustered["mean"],
            "mean_pnl_taker": (sum(taker_values) / len(taker_values)
                               if taker_values else None),
            "effective_n": clustered["effective_n"],
            "se_pessimistic": clustered["se"],
            "t_pessimistic": clustered["t"],
            "ci95_pessimistic": clustered["ci95"],
            "walkforward_pessimistic": walkforward_holdout_mean(pess_pairs),
        }
        p_value = _two_sided_p(clustered["t"])
        cell["p_value"] = p_value
        pvals.append(p_value)
        cells.append(cell)

    rejected = benjamini_hochberg(pvals, alpha=BH_ALPHA) if pvals else []
    certified_count = 0
    for cell, is_rejected in zip(cells, rejected):
        cell["bh_rejected"] = is_rejected
        cell["certified"] = certify(cell)
        if cell["certified"]:
            certified_count += 1

    return {
        "schema_version": SCHEMA_VERSION,
        "as_of_utc": common.as_of(),
        "data": {
            "brti_inventory": brti_inventory,
            "brti_samples": len(brti),
            "quote_inventory": quote_inventory,
            "quote_rows_dropped": quote_dropped,
            "quote_markets": len(quotes),
            "trade_tickers": len(trades),
            "trade_rows": sum(len(v) for v in trades.values()),
            "size_tickers": len(sizes),
            "size_rows_dropped": sizes_dropped,
            "settlement_inventory": settlement_inventory,
            "markets_with_resolved_outcome": len(outcomes),
            "outcome_source_counts": dict(outcome_source),
            "markets_unresolved": unresolved,
            "paired_window_utc": [common.iso(start), common.iso(end)],
            "paired_window_hours": paired_hours,
            "events_detected_by_threshold": events_by_threshold,
            "cell_count": len(cells),
            "certified_cell_count": certified_count,
        },
        "method": {
            "event_detector": "q1_quote_lag.detect_events, sigma thresholds %s"
                              % (list(q1.SIGMA_THRESHOLDS),),
            "rest_offsets_ticks": list(REST_OFFSET_TICKS),
            "exit_horizons_s": list(EXIT_HORIZONS_S),
            "fill_model": ("bracketed: optimistic = first taker print through "
                           "our price after join; pessimistic = the print at "
                           "which cumulative taker volume through our price "
                           "first exceeds the real top-of-book size ahead of "
                           "us at join (maker_fill)"),
            "cell_key": "(threshold_sigma, side, rest_offset_ticks, horizon_s)",
            "cluster_key": "record['cluster'] (the triggering event's ts_ms)",
            "clustered_stats": ("clustered_mean(pnl_pessimistic, cluster): "
                                "effective_n = n^2 / sum(cluster_size^2); "
                                "se is the cluster-robust (CR1) standard error"),
            "correction": ("Benjamini-Hochberg over every cell's p-value on "
                           "pnl_pessimistic, alpha=%.2f" % BH_ALPHA),
            "walkforward": ("mean pnl_pessimistic over the final ~20% of "
                            "events by time (5-fold boundary), compared by "
                            "sign against the full-sample mean"),
            "certify_rule": (
                "mean_pnl_pessimistic > 0 AND bh_rejected AND "
                "sign(walkforward_pessimistic) == sign(mean_pnl_pessimistic) "
                "AND effective_n >= %d. mean_pnl_optimistic and both fill "
                "rates are reported for context and never gate certification."
                % CERTIFY_MIN_EFFECTIVE_N),
        },
        "cells": cells,
    }


def _atomic_write_json(path, payload):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True, default=str)
    os.replace(tmp, path)


def _render_report(full):
    data = full["data"]
    certified = [c for c in full["cells"] if c["certified"]]
    certified.sort(key=lambda c: c["mean_pnl_pessimistic"] or 0.0, reverse=True)
    if certified:
        headline = ("%d cell%s certify a real maker edge under the "
                    "pessimistic fill bound"
                    % (len(certified), "" if len(certified) == 1 else "s"))
    elif data["cell_count"] == 0:
        headline = "no sigma-events in the retained window -- insufficient sample"
    else:
        headline = "no cell certifies -- fee-eaten (or insufficient sample) even as a maker"
    lines = [
        "# Kalshi maker quote-lag — %s" % full["as_of_utc"],
        "",
        "paired window: %s (%.2fh)" % (data["paired_window_utc"], data["paired_window_hours"]),
        "sigma-events detected: %s" % data["events_detected_by_threshold"],
        "cells swept: %d" % data["cell_count"],
        "markets with resolved outcome: %d (unresolved: %d)"
        % (data["markets_with_resolved_outcome"], data["markets_unresolved"]),
        "",
        "## %s" % headline,
    ]
    for c in certified:
        lines.append(
            "  - sigma=%.1f side=%s offset=%dtick horizon=%ds  "
            "mean_pnl_pessimistic=%.4f  effective_n=%.1f  fill_rate_pess=%.2f"
            % (c["threshold_sigma"], c["side"], c["rest_offset_ticks"], c["horizon_s"],
               c["mean_pnl_pessimistic"], c["effective_n"], c["fill_rate_pessimistic"] or 0.0))
    return "\n".join(lines)


def main():
    full = run(common.evidence_dir())
    _atomic_write_json(common.evidence_path("maker-quote-lag.json"), full)
    print(_render_report(full))
    return 0


if __name__ == "__main__":
    sys.exit(main())
