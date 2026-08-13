# Kalshi structural-arbitrage recorder

Status: read-only measurement. It cannot place, cancel, or modify orders.

## What it measures

Kalshi exposes one reciprocal binary book. A YES bid at `p` is a NO ask at
`1-p`, and a NO bid at `p` is a YES ask at `1-p`. Therefore two displayed bids
whose sum is below $1 are not a buy-both opportunity. The scanner derives asks
only from the opposite bids and sweeps actual displayed depth.

The useful Kalshi question is whether a **reviewed bundle of different
contracts** has a guaranteed minimum payout greater than its executable cost,
current taker fees, and a conservative legging/slippage buffer.

```text
net profit = minimum certified payout
           - depth-weighted acquisition cost
           - series-derived taker fees
           - execution buffer
```

It reports `opportunity`, `not_profitable`, or `unavailable`. Missing books,
insufficient equal-sized depth, closed markets, missing series metadata, and
unsupported fee types are `unavailable`, never zero-cost opportunities.

## Payoff certificates

The scanner does not infer relationships from titles. Every bundle requires a
reviewed JSON certificate enumerating every allowed settlement outcome and the
0/1 payout of every acquired leg in each outcome. Version 1 refuses a matrix
whose minimum payout is zero.

Example structure (illustrative tickers only; not a live certificate):

```json
{
  "schema_version": 1,
  "bundle_id": "reviewed-three-range-buckets",
  "description": "Exactly one reviewed range bucket settles YES",
  "outcomes": ["low", "middle", "high"],
  "legs": [
    {"ticker": "REPLACE-LOW", "side": "yes", "payouts": [1, 0, 0]},
    {"ticker": "REPLACE-MIDDLE", "side": "yes", "payouts": [0, 1, 0]},
    {"ticker": "REPLACE-HIGH", "side": "yes", "payouts": [0, 0, 1]}
  ]
}
```

Do not preregister a certificate until the contracts' official settlement
rules establish that the enumerated states are exhaustive. Correlation,
similar wording, or shared underliers are insufficient.

## Commands

Discover machine-declared event candidates for manual settlement-rule review:

```bash
python3 scripts/kalshi_structural_arb.py discover \
  --out logs/kalshi-structural-arb-candidates.jsonl
```

Discovery stores Kalshi's full event and child-market metadata plus a canonical
digest. It **never** emits a payoff certificate. In particular,
`mutually_exclusive=true` does not establish exhaustive binary settlement:
Kalshi documents that some contracts can receive scalar, last-fair-price, or
other non-standard payouts. Every discovered row therefore remains
`manual_rules_review_required` until those states are explicitly represented
in a reviewed certificate.

One authenticated read-only snapshot:

```bash
python3 scripts/kalshi_structural_arb.py scan reviewed-certificate.json \
  --quantity 1 --execution-buffer 0.01 --min-net-edge 0.01 \
  --out logs/kalshi-structural-arb.jsonl
```

Repeated evidence capture:

```bash
python3 scripts/kalshi_structural_arb.py record reviewed-certificate.json \
  --seconds 3600 --poll-seconds 1 \
  --quantity 1 --execution-buffer 0.01 --min-net-edge 0.01 \
  --out logs/kalshi-structural-arb.jsonl
```

Offline audit/replay:

```bash
python3 scripts/kalshi_structural_arb.py replay logs/kalshi-structural-arb.jsonl
```

Replay recomputes the evaluation from the recorded certificate, batch books,
fee schedules, quantity, and buffers. It fails if the certificate digest or a
recorded result was changed.

## BTC threshold-corridor family

`btc_threshold_corridor` is a distinct structural-measurement family. It is
not `KXBTC15M`, does not make a directional Bitcoin prediction, and cannot
inherit a directional model's admission verdict. For two thresholds `L < H`
with identical reviewed settlement terms, it measures equal-sized purchases
of:

- YES on "BTC at/above L"; and
- NO on "BTC at/above H".

The certified payout is $1 below `L`, $2 in the corridor `[L,H)`, and $1 at or
above `H`. Thus the conservative guaranteed payout is $1; the extra dollar in
the middle is upside, not part of the arbitrage test. With strict `>` contracts
the boundary labels change, and the certificate must say so explicitly.

The family certificate binds all members of one reviewed threshold ladder:

- underlier `BTC`, exact event ticker, API strike type and comparison;
- exact settlement-time field and value;
- every ticker and strike; and
- a SHA-256 of each market's settlement/strike terms.

Both `rules_reviewed` and `ordinary_binary_payouts_only` must be explicitly
true. Titles, similar wording, and proximity in time never create a
certificate. A change to any bound term makes the entire family unavailable.
This is deliberately strict because scalar payouts, cancellations, differing
price sources, or mismatched boundary rules can destroy the payoff proof.

Illustrative schema (hashes and tickers are placeholders, not certification):

```json
{
  "schema_version": 1,
  "family": "btc_threshold_corridor",
  "underlier": "BTC",
  "rules_reviewed": true,
  "ordinary_binary_payouts_only": true,
  "event_ticker": "REPLACE-EVENT",
  "comparison": "greater_than_or_equal",
  "api_strike_type": "greater_equal",
  "settlement_time_field": "expected_expiration_time",
  "settlement_time": "REPLACE-EXACT-API-VALUE",
  "reviewed_at": "REPLACE-REVIEW-TIME",
  "markets": [
    {"ticker": "REPLACE-LOW", "strike": "64000", "terms_sha256": "REPLACE-64-HEX"},
    {"ticker": "REPLACE-HIGH", "strike": "65000", "terms_sha256": "REPLACE-64-HEX"}
  ]
}
```

One scan fetches the ladder's books in a single batch, enumerates every
lower-YES/higher-NO pair, sweeps equal displayed depth, and records every pair
under the family name:

```bash
python3 scripts/kalshi_structural_arb.py corridor-scan reviewed-btc-ladder.json \
  --quantity 1 --execution-buffer 0.01 --min-net-edge 0.01 \
  --out logs/btc-threshold-corridor.jsonl

python3 scripts/kalshi_structural_arb.py corridor-record reviewed-btc-ladder.json \
  --seconds 3600 --poll-seconds 1 \
  --quantity 1 --execution-buffer 0.01 --min-net-edge 0.01 \
  --out logs/btc-threshold-corridor.jsonl
```

The commands remain read-only. No order operation is exposed. A positive row
is evidence for latency, duration and partial-fill research—not permission to
trade and not proof that two legs can fill atomically.

## Evidence and limitations

- Books are fetched from Kalshi's multi-market batch endpoint, avoiding one
  sequential REST request per leg. The API does not publish an exchange-level
  atomic multi-contract execution primitive here; the buffer is not a proof of
  simultaneous fills.
- Fees are taken from each market's current series `fee_type` and
  `fee_multiplier`. Version 1 supports the current quadratic taker schedule and
  refuses unknown or flat schedules rather than guessing.
- Fee rounding is applied conservatively at every swept price level.
- A positive snapshot is a research observation, not permission to trade.
- No live path should be designed until recorded opportunities survive
  duration, latency, partial-fill, and sequence-integrity analysis.
