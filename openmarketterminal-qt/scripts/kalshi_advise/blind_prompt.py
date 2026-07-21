"""Frozen, byte-identical blind forecasting prompt shared by every contestant."""
import hashlib
import json

PROMPT_VERSION = "kalshi-blind-shared-v1"
INSTRUCTION = (
    "You are a disciplined probabilistic forecaster for ONE Kalshi crypto settlement "
    "contract. You are given ONLY price-free context (JSON). You do NOT see the contract "
    "price, market-implied probability, contract order flow, or any model output. Estimate "
    "the probability the contract settles YES using only the supplied context. Respond with "
    "ONLY one JSON object: either "
    '{"decision":"predict","probability":<0..1>,"confidence":<0..1>,'
    '"rationale":"<one sentence, no price references>"} or '
    '{"decision":"abstain","reason_code":"INSUFFICIENT_EVIDENCE",'
    '"confidence":<0..1>,"rationale":"<one sentence>"}.'
)


def canonical_context(context):
    return json.dumps(context, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def build_prompt(context):
    return INSTRUCTION + "\n\nContext:\n" + canonical_context(context)


def prompt_hash(context):
    return hashlib.sha256(build_prompt(context).encode("utf-8")).hexdigest()


def competition_context(context):
    """Bound the shared model packet without admitting any market-derived key."""
    top = ("strike_floor", "strike_cap", "spot", "distance_bps", "required_move_bps",
           "seconds_left", "settlement_band", "settlement_def", "horizon",
           "realized_move_bps", "realized_volatility")
    compact = {key: context[key] for key in top if key in context}
    spot = context.get("spot_microstructure")
    if isinstance(spot, dict):
        allowed = ("direction", "confidence", "tape_pressure", "book_pressure",
                   "aggressor_pressure", "aggressor_coverage", "live_sources",
                   "top_book_sources", "cross_source_spread_bps", "reference_price",
                   "freshest_age_ms", "freshest_source")
        compact["spot_microstructure"] = {key: spot[key] for key in allowed if key in spot}
    return compact
