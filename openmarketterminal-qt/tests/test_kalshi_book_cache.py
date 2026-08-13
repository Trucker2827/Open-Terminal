from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from scripts.kalshi_microstructure.book_cache import KalshiBookCache


def snapshot(seq: int | None, *, yes=(), no=()) -> dict:
    message = {
        "type": "orderbook_snapshot",
        "msg": {
            "market_ticker": "BTC",
            "yes_dollars": [[str(price), str(size)] for price, size in yes],
            "no_dollars": [[str(price), str(size)] for price, size in no],
        },
    }
    if seq is not None:
        message["seq"] = seq
    return message


def delta(seq: int | None, *, side: str = "yes", price: str = "0.40", size: str = "1") -> dict:
    message = {
        "type": "orderbook_delta",
        "msg": {
            "market_ticker": "BTC",
            "side": side,
            "price_dollars": price,
            "delta_fp": size,
        },
    }
    if seq is not None:
        message["seq"] = seq
    return message


class KalshiBookCacheTest(unittest.TestCase):
    def test_current_dollar_snapshot_fields_and_contiguous_delta(self) -> None:
        cache = KalshiBookCache("BTC")
        cache.apply(snapshot(10, yes=(("0.40", "2"),), no=(("0.55", "3"),)))
        cache.apply(delta(11, price="0.40", size="1"))
        self.assertTrue(cache.valid)
        self.assertEqual(cache.seq, 11)
        self.assertEqual(cache.yes, {0.4: 3.0})
        self.assertEqual(cache.no, {0.55: 3.0})

    def test_sequence_gap_invalidates_and_empties_the_book(self) -> None:
        cache = KalshiBookCache("BTC")
        cache.apply(snapshot(10, yes=(("0.40", "2"),)))
        cache.apply(delta(12))
        self.assertFalse(cache.valid)
        self.assertIsNone(cache.seq)
        self.assertEqual(cache.yes, {})
        self.assertEqual(cache.no, {})
        self.assertEqual(cache.gap_reason, "orderbook sequence gap: expected 11, got 12")

        # Later deltas cannot silently heal a corrupted ladder.
        cache.apply(delta(13))
        self.assertFalse(cache.valid)
        self.assertEqual(cache.yes, {})

        # Only a fresh snapshot establishes a new coherent baseline.
        cache.apply(snapshot(20, yes=(("0.42", "4"),)))
        self.assertTrue(cache.valid)
        self.assertEqual(cache.seq, 20)
        self.assertEqual(cache.yes, {0.42: 4.0})

    def test_missing_sequence_never_becomes_a_usable_book(self) -> None:
        cache = KalshiBookCache("BTC")
        cache.apply(snapshot(None, yes=(("0.40", "2"),)))
        self.assertFalse(cache.valid)
        self.assertIsNone(cache.seq)
        self.assertEqual(cache.yes, {})
        self.assertIn("missing sequence", cache.gap_reason)


if __name__ == "__main__":
    unittest.main()
