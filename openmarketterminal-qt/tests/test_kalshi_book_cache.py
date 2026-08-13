from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from scripts.kalshi_microstructure.book_cache import (
    KalshiBookCache,
    KalshiSubscriptionBookCache,
    ReconnectRequired,
)


def snapshot(seq: int | None, *, ticker: str = "BTC", sid: int = 7, ts_ms: int | None = None,
             yes=(), no=()) -> dict:
    message = {
        "type": "orderbook_snapshot",
        "msg": {
            "market_ticker": ticker,
            "yes_dollars": [[str(price), str(size)] for price, size in yes],
            "no_dollars": [[str(price), str(size)] for price, size in no],
        },
    }
    if seq is not None:
        message["seq"] = seq
    message["sid"] = sid
    if ts_ms is not None:
        message["msg"]["ts_ms"] = ts_ms
    return message


def delta(seq: int | None, *, ticker: str = "BTC", sid: int = 7, ts_ms: int | None = None,
          side: str = "yes", price: str = "0.40", size: str = "1") -> dict:
    message = {
        "type": "orderbook_delta",
        "msg": {
            "market_ticker": ticker,
            "side": side,
            "price_dollars": price,
            "delta_fp": size,
        },
    }
    if seq is not None:
        message["seq"] = seq
    message["sid"] = sid
    if ts_ms is not None:
        message["msg"]["ts_ms"] = ts_ms
    return message


def control(seq: int, *, sid: int = 7) -> dict:
    return {"type": "ok", "sid": sid, "seq": seq, "msg": {"command": "update_subscription"}}


class KalshiBookCacheTest(unittest.TestCase):
    def test_subscribed_frame_is_outside_sequence(self) -> None:
        cache = KalshiSubscriptionBookCache(("A",))
        cache.apply({"type": "subscribed", "sid": None, "seq": None, "msg": {}})
        self.assertTrue(cache.valid)
        self.assertIsNone(cache.sid)
        self.assertIsNone(cache.seq)

    def test_current_dollar_snapshot_fields_and_contiguous_delta(self) -> None:
        cache = KalshiBookCache("BTC", validate_sequence=True)
        cache.apply(snapshot(10, yes=(("0.40", "2"),), no=(("0.55", "3"),)))
        cache.apply(delta(11, price="0.40", size="1"))
        self.assertTrue(cache.valid)
        self.assertEqual(cache.seq, 11)
        self.assertEqual(cache.yes, {0.4: 3.0})
        self.assertEqual(cache.no, {0.55: 3.0})

    def test_sequence_gap_invalidates_and_empties_the_book(self) -> None:
        cache = KalshiBookCache("BTC", validate_sequence=True)
        cache.apply(snapshot(10, yes=(("0.40", "2"),)))
        with self.assertRaisesRegex(ReconnectRequired, "expected 11, got 12"):
            cache.apply(delta(12))
        self.assertFalse(cache.valid)
        self.assertEqual(cache.seq, 10)
        self.assertEqual(cache.yes, {})
        self.assertEqual(cache.no, {})
        self.assertEqual(cache.gap_reason, "subscription sequence gap: expected 11, got 12")

        # Later deltas cannot silently heal a corrupted ladder.
        # The transport must tear down this connection. Recovery is a new
        # cache on a new connection, because re-subscribe yields no snapshot.
        replacement = KalshiBookCache("BTC", validate_sequence=True)
        replacement.apply(snapshot(1, sid=8, yes=(("0.42", "4"),)))
        self.assertTrue(replacement.valid)
        self.assertEqual(replacement.seq, 1)
        self.assertEqual(replacement.yes, {0.42: 4.0})

    def test_missing_sequence_never_becomes_a_usable_book(self) -> None:
        cache = KalshiBookCache("BTC", validate_sequence=True)
        with self.assertRaisesRegex(ReconnectRequired, "missing sid or seq"):
            cache.apply(snapshot(None, yes=(("0.40", "2"),)))
        self.assertFalse(cache.valid)
        self.assertIsNone(cache.seq)
        self.assertEqual(cache.yes, {})
        self.assertIn("missing sid or seq", cache.gap_reason)

    def test_multi_market_sequence_is_global_not_per_market(self) -> None:
        cache = KalshiSubscriptionBookCache(("A", "B", "C"))
        cache.apply(snapshot(1, ticker="A", ts_ms=1000, yes=(("0.40", "2"),)))
        cache.apply(snapshot(2, ticker="B", ts_ms=1001, yes=(("0.41", "2"),)))
        cache.apply(snapshot(3, ticker="C", ts_ms=1002, yes=(("0.42", "2"),)))
        cache.apply(delta(4, ticker="C", ts_ms=1003))
        cache.apply(delta(5, ticker="B", ts_ms=1004))
        cache.apply(delta(6, ticker="C", ts_ms=1005))
        cache.apply(delta(7, ticker="A", ts_ms=1006))
        self.assertTrue(cache.ready)
        self.assertEqual(cache.seq, 7)
        self.assertEqual(cache.books["A"].seq, 7)  # 1 -> 7 is valid for market A.
        self.assertEqual(cache.books["B"].seq, 5)
        self.assertEqual(cache.books["C"].seq, 6)
        self.assertTrue(cache.coherent_within(2))
        self.assertFalse(cache.coherent_within(1))

    def test_control_frame_advances_shared_sequence(self) -> None:
        cache = KalshiSubscriptionBookCache(("A", "B"))
        cache.apply(snapshot(1, ticker="A", yes=(("0.40", "2"),)))
        cache.apply(snapshot(2, ticker="B", yes=(("0.41", "2"),)))
        cache.apply(control(3))
        cache.apply(delta(4, ticker="A"))
        self.assertTrue(cache.ready)
        self.assertEqual(cache.seq, 4)

        single = KalshiBookCache("BTC", validate_sequence=True)
        single.apply(snapshot(10, yes=(("0.40", "2"),)))
        single.apply(control(11))
        single.apply(delta(12))
        self.assertTrue(single.valid)
        self.assertEqual(single.seq, 12)

    def test_subscription_gap_invalidates_every_market(self) -> None:
        cache = KalshiSubscriptionBookCache(("A", "B"))
        cache.apply(snapshot(20, ticker="A", yes=(("0.40", "2"),)))
        cache.apply(snapshot(21, ticker="B", yes=(("0.41", "2"),)))
        with self.assertRaisesRegex(ReconnectRequired, "expected 22, got 23"):
            cache.apply(delta(23, ticker="A"))
        self.assertFalse(cache.valid)
        self.assertEqual(cache.gap_reason, "subscription sequence gap: expected 22, got 23")
        self.assertEqual(cache.to_books(), {})
        self.assertFalse(cache.books["A"].valid)
        self.assertFalse(cache.books["B"].valid)

    def test_subscription_refuses_delta_before_that_markets_snapshot(self) -> None:
        cache = KalshiSubscriptionBookCache(("A", "B"))
        cache.apply(snapshot(1, ticker="A", yes=(("0.40", "2"),)))
        with self.assertRaisesRegex(ReconnectRequired, "delta preceded snapshot"):
            cache.apply(delta(2, ticker="B"))
        self.assertFalse(cache.valid)
        self.assertEqual(cache.gap_reason, "orderbook delta preceded snapshot: B")

    def test_malformed_message_invalidates_and_requires_reconnect(self) -> None:
        cache = KalshiSubscriptionBookCache(("A",))
        with self.assertRaisesRegex(ReconnectRequired, "malformed orderbook snapshot"):
            cache.apply(snapshot(1, ticker="A", yes=(("not-a-price", "2"),)))
        self.assertFalse(cache.valid)
        self.assertIn("malformed orderbook snapshot", cache.gap_reason)

        direct = KalshiBookCache("BTC", validate_sequence=True)
        direct.apply(snapshot(1, yes=(("0.40", "2"),)))
        with self.assertRaisesRegex(ReconnectRequired, "unrecognised side"):
            direct.apply(delta(2, side="renamed-side"))
        self.assertFalse(direct.valid)
        self.assertIn("unrecognised side", direct.gap_reason)

        bad_seq = KalshiBookCache("BTC", validate_sequence=True)
        with self.assertRaisesRegex(ReconnectRequired, "malformed subscription sequence"):
            bad_seq.apply(snapshot("not-an-int", yes=(("0.40", "2"),)))
        self.assertFalse(bad_seq.valid)
        self.assertIn("malformed subscription sequence", bad_seq.gap_reason)


if __name__ == "__main__":
    unittest.main()
