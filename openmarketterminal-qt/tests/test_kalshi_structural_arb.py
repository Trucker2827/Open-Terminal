from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest
from decimal import Decimal

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from scripts.kalshi_microstructure.kalshi import KalshiRestClient
from scripts.kalshi_microstructure.models import BinaryBook, Level
from scripts.kalshi_microstructure.structural_arb import (
    FeeSchedule,
    PayoffCertificate,
    ReadOnlyKalshiClient,
    collect_snapshot,
    evaluate_bundle,
    replay_evidence,
)


def certificate(payload: dict) -> PayoffCertificate:
    return PayoffCertificate.from_payload({"schema_version": 1, **payload})


def book(ticker: str, *, yes_bids=(), no_bids=()) -> BinaryBook:
    return BinaryBook(
        ticker=ticker,
        yes_bids=tuple(Level(price=price, size=size) for price, size in yes_bids),
        no_bids=tuple(Level(price=price, size=size) for price, size in no_bids),
    )


class StructuralArbitrageTest(unittest.TestCase):
    def test_same_market_yes_no_uses_reciprocal_asks_not_bid_sum(self) -> None:
        cert = certificate(
            {
                "bundle_id": "binary-complete-set",
                "description": "One YES and one NO on the exact same binary market",
                "outcomes": ["yes_resolves", "no_resolves"],
                "legs": [
                    {"ticker": "KXTEST", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "KXTEST", "side": "no", "payouts": [0, 1]},
                ],
            }
        )
        # The viral-arbitrage mistake calls 0.47 + 0.49 = 0.96.  On Kalshi
        # those are bids.  Executable asks are 1-0.49=0.51 and 1-0.47=0.53.
        books = {"KXTEST": book("KXTEST", yes_bids=((0.47, 10),), no_bids=((0.49, 10),))}
        result = evaluate_bundle(
            cert,
            books,
            {"KXTEST": FeeSchedule("none", Decimal("1"))},
        )
        self.assertEqual(result.state, "not_profitable")
        self.assertEqual(result.acquisition_cost, Decimal("1.04"))
        self.assertEqual(result.net_profit, Decimal("-0.04"))

    def test_exhaustive_one_hot_bundle_can_be_an_opportunity(self) -> None:
        cert = certificate(
            {
                "bundle_id": "three-buckets",
                "description": "Exactly one reviewed range bucket settles YES",
                "outcomes": ["low", "middle", "high"],
                "legs": [
                    {"ticker": "LOW", "side": "yes", "payouts": [1, 0, 0]},
                    {"ticker": "MID", "side": "yes", "payouts": [0, 1, 0]},
                    {"ticker": "HIGH", "side": "yes", "payouts": [0, 0, 1]},
                ],
            }
        )
        books = {
            "LOW": book("LOW", no_bids=((0.75, 10),)),
            "MID": book("MID", no_bids=((0.72, 10),)),
            "HIGH": book("HIGH", no_bids=((0.70, 10),)),
        }
        fees = {ticker: FeeSchedule("none", Decimal("1")) for ticker in books}
        result = evaluate_bundle(
            cert,
            books,
            fees,
            execution_buffer_per_contract=Decimal("0.01"),
            min_net_edge_per_bundle=Decimal("0.10"),
        )
        self.assertEqual(result.state, "opportunity")
        self.assertEqual(result.guaranteed_payout, Decimal("1"))
        self.assertEqual(result.acquisition_cost, Decimal("0.83"))
        self.assertEqual(result.execution_buffer, Decimal("0.03"))
        self.assertEqual(result.net_profit, Decimal("0.14"))

    def test_certificate_refuses_non_exhaustive_payoff_matrix(self) -> None:
        with self.assertRaisesRegex(ValueError, "positive payout in every outcome"):
            certificate(
                {
                    "bundle_id": "not-exhaustive",
                    "description": "Both legs lose in the omitted middle state",
                    "outcomes": ["low", "middle", "high"],
                    "legs": [
                        {"ticker": "LOW", "side": "yes", "payouts": [1, 0, 0]},
                        {"ticker": "HIGH", "side": "yes", "payouts": [0, 0, 1]},
                    ],
                }
            )

    def test_certificate_refuses_non_complementary_same_market_sides(self) -> None:
        with self.assertRaisesRegex(ValueError, "YES/NO payouts must be complementary"):
            certificate(
                {
                    "bundle_id": "impossible-double-payout",
                    "description": "A malformed certificate cannot make both sides pay",
                    "outcomes": ["yes_resolves", "no_resolves"],
                    "legs": [
                        {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                        {"ticker": "A", "side": "no", "payouts": [1, 1]},
                    ],
                }
            )

    def test_missing_book_depth_is_unavailable_not_a_zero_cost_opportunity(self) -> None:
        cert = certificate(
            {
                "bundle_id": "depth",
                "description": "Depth must exist on every leg",
                "outcomes": ["a", "b"],
                "legs": [
                    {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "B", "side": "yes", "payouts": [0, 1]},
                ],
            }
        )
        result = evaluate_bundle(
            cert,
            {"A": book("A", no_bids=((0.80, 10),)), "B": book("B")},
            {"A": FeeSchedule("none", Decimal("1")), "B": FeeSchedule("none", Decimal("1"))},
        )
        self.assertEqual(result.state, "unavailable")
        self.assertIn("insufficient YES ask depth: B", result.reason)

    def test_conservative_fees_can_erase_a_gross_gap(self) -> None:
        cert = certificate(
            {
                "bundle_id": "fees",
                "description": "A raw discount is not automatically net profit",
                "outcomes": ["a", "b"],
                "legs": [
                    {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "B", "side": "yes", "payouts": [0, 1]},
                ],
            }
        )
        books = {
            "A": book("A", no_bids=((0.51, 10),)),  # ask 0.49
            "B": book("B", no_bids=((0.51, 10),)),
        }
        fee = FeeSchedule("quadratic", Decimal("1"))
        result = evaluate_bundle(cert, books, {"A": fee, "B": fee})
        self.assertEqual(result.acquisition_cost, Decimal("0.98"))
        self.assertEqual(result.fees, Decimal("0.04"))
        self.assertEqual(result.state, "not_profitable")
        self.assertEqual(result.net_profit, Decimal("-0.02"))

    def test_unsupported_fee_type_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported fee_type"):
            FeeSchedule.from_payload({"fee_type": "flat", "fee_multiplier": 1})

    def test_collection_failure_is_recorded_and_never_calls_order_api(self) -> None:
        cert = certificate(
            {
                "bundle_id": "read-only",
                "description": "The collector has no execution dependency",
                "outcomes": ["a", "b"],
                "legs": [
                    {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "B", "side": "yes", "payouts": [0, 1]},
                ],
            }
        )

        class ReadOnlyProbe:
            order_calls = 0

            def get_market(self, ticker):
                raise TimeoutError("feed timed out")

            def create_order(self, payload):
                self.order_calls += 1
                raise AssertionError("read-only scanner called create_order")

        probe = ReadOnlyProbe()
        row = collect_snapshot(probe, cert)
        self.assertEqual(row["evaluation"]["state"], "unavailable")
        self.assertIn("collection failed: TimeoutError", row["collection_error"])
        self.assertEqual(probe.order_calls, 0)

        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "unavailable.jsonl"
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            self.assertTrue(replay_evidence(path)["valid"])

    def test_depth_sweep_uses_vwap_and_requires_full_equal_quantity(self) -> None:
        cert = certificate(
            {
                "bundle_id": "vwap",
                "description": "Equal-sized legs with multiple price levels",
                "outcomes": ["a", "b"],
                "legs": [
                    {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "B", "side": "yes", "payouts": [0, 1]},
                ],
            }
        )
        books = {
            "A": book("A", no_bids=((0.80, 1), (0.70, 2))),  # asks .20 x1, .30 x2
            "B": book("B", no_bids=((0.60, 3),)),            # ask .40 x3
        }
        fees = {ticker: FeeSchedule("none", Decimal("1")) for ticker in books}
        result = evaluate_bundle(cert, books, fees, quantity=Decimal("2"))
        self.assertEqual(result.state, "opportunity")
        self.assertEqual(result.acquisition_cost, Decimal("1.30"))
        self.assertEqual(result.guaranteed_payout, Decimal("2"))
        unavailable = evaluate_bundle(cert, books, fees, quantity=Decimal("4"))
        self.assertEqual(unavailable.state, "unavailable")

    def test_replay_detects_tampered_evaluation(self) -> None:
        cert = certificate(
            {
                "bundle_id": "replay",
                "description": "Replay fixture",
                "outcomes": ["a", "b"],
                "legs": [
                    {"ticker": "A", "side": "yes", "payouts": [1, 0]},
                    {"ticker": "B", "side": "yes", "payouts": [0, 1]},
                ],
            }
        )
        books = {
            "A": book("A", no_bids=((0.75, 10),)),
            "B": book("B", no_bids=((0.75, 10),)),
        }
        fees = {ticker: FeeSchedule("none", Decimal("1")) for ticker in books}
        evaluation = evaluate_bundle(cert, books, fees).payload()
        row = {
            "certificate": cert.payload(),
            "certificate_sha256": cert.digest,
            "quantity": "1",
            "execution_buffer_per_contract": "0",
            "min_net_edge_per_bundle": "0",
            "books": {
                ticker: {
                    "ticker": ticker,
                    "yes_bids": [],
                    "no_bids": [[str(level.price), str(level.size)] for level in value.no_bids],
                }
                for ticker, value in books.items()
            },
            "fees": {ticker: schedule.payload() for ticker, schedule in fees.items()},
            "evaluation": evaluation,
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "evidence.jsonl"
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            self.assertTrue(replay_evidence(path)["valid"])
            row["evaluation"]["net_profit"] = "999"
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            report = replay_evidence(path)
            self.assertFalse(report["valid"])
            self.assertEqual(report["mismatches"], 1)


class BatchOrderbooksTest(unittest.TestCase):
    def test_read_only_facade_has_no_mutation_methods(self) -> None:
        facade = ReadOnlyKalshiClient(KalshiRestClient())
        self.assertFalse(hasattr(facade, "create_order"))
        self.assertFalse(hasattr(facade, "cancel_order"))
        self.assertEqual(
            {name for name in dir(facade) if name.startswith("get_")},
            {"get_market", "get_orderbooks", "get_series"},
        )

    def test_client_parses_one_batch_snapshot(self) -> None:
        class FakeClient:
            get_orderbooks = KalshiRestClient.get_orderbooks

            def __init__(self) -> None:
                self.calls = []

            def get_json(self, path, params):
                self.calls.append((path, params))
                return {
                    "orderbooks": [
                        {
                            "ticker": "A",
                            "orderbook_fp": {
                                "yes_dollars": [["0.4000", "2.00"]],
                                "no_dollars": [["0.5500", "3.00"]],
                            },
                        }
                    ]
                }

        client = FakeClient()
        books = client.get_orderbooks(["A"])
        self.assertEqual(client.calls, [("/markets/orderbooks", {"tickers": ["A"]})])
        self.assertEqual(books["A"].best_yes_ask.price, 0.45)
        self.assertEqual(books["A"].best_yes_ask.size, 3.0)


if __name__ == "__main__":
    unittest.main()
