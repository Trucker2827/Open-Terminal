#!/usr/bin/env python3
"""Kalshi maker quote-lag engine (read-only). Measures whether RESTING a quote to
capture the book-lag after a >=sigma BTC move is a real edge, with a fill model that
BRACKETS queue position between optimistic (front) and pessimistic (back) bounds
computed from real trade prints. Certifies an edge only under the pessimistic bound.
NO live execution, ever. See docs/design/2026-07-30-kalshi-maker-quote-lag-design.md.

    python3 scripts/research/maker_quote_lag.py
"""
import bisect, collections, json, math, sys
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
# hidden one) when none was.
AHEAD_SIZE_SOURCE_TOP_OF_BOOK = "top_of_book"
AHEAD_SIZE_SOURCE_NO_SIZE_ROW = "no_size_row"


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

        size_row = sizes.at(ticker, t) if sizes is not None else None
        if size_row is None:
            ahead_size = 0.0
            ahead_size_source = AHEAD_SIZE_SOURCE_NO_SIZE_ROW
        else:
            _, yes_bid_size, yes_ask_size = size_row
            # RESTING side at join: a YES bet posts a YES bid (ahead ==
            # yes_bid_size); a NO bet posts a NO bid, which sits on the YES
            # ASK side of the same book (ahead == yes_ask_size).
            ahead_size = yes_bid_size if side == "YES" else yes_ask_size
            ahead_size_source = AHEAD_SIZE_SOURCE_TOP_OF_BOOK

        for offset_ticks in REST_OFFSET_TICKS:
            rest_price = round(touch_bid + offset_ticks * TICK_SIZE, 4)
            if rest_price >= touch_ask:
                continue  # would cross the spread -- not a restable maker price
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
