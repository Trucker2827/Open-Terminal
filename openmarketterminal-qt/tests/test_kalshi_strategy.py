from datetime import datetime, timedelta, timezone
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from scripts.kalshi_microstructure.models import BinaryBook, Level, Market
from scripts.kalshi_microstructure.strategy import find_edge


class KalshiStrategyTest(unittest.TestCase):
    def test_find_edge_rejects_market_at_or_after_close(self) -> None:
        now = datetime(2026, 7, 13, 12, 0, tzinfo=timezone.utc)
        book = BinaryBook(
            ticker="KXBTC-TEST",
            yes_bids=(Level(price=0.01, size=100),),
            no_bids=(Level(price=0.01, size=100),),
        )

        for close_time in (now, now - timedelta(seconds=1)):
            market = Market(
                ticker="KXBTC-TEST",
                title="Will BTC be above $64000?",
                subtitle="",
                status="open",
                close_time=close_time,
                raw={"floor_strike": 64000},
            )
            self.assertIsNone(
                find_edge(
                    market=market,
                    book=book,
                    spot=65000,
                    now=now,
                    min_edge=0.0,
                )
            )


if __name__ == "__main__":
    unittest.main()
