import math
import os
import sqlite3
import struct
import sys
import tempfile
import unittest

SCRIPTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts", "ai_quant_lab"))
sys.path.insert(0, SCRIPTS)
import crypto_dataset as cd


def read_bin(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    n = len(raw) // 4 - 1
    start = struct.unpack("<f", raw[:4])[0]
    values = struct.unpack(f"<{n}f", raw[4:])
    return int(start), list(values)


class CryptoDatasetTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "ticks.db")
        self.out = os.path.join(self.tmp.name, "crypto_data")
        conn = sqlite3.connect(self.db)
        conn.execute("""CREATE TABLE edge_prediction_raw_ticks (
            id TEXT PRIMARY KEY, symbol TEXT NOT NULL DEFAULT '',
            source TEXT NOT NULL DEFAULT '', price REAL NOT NULL DEFAULT 0,
            exchange_ts INTEGER NOT NULL DEFAULT 0, received_ts INTEGER NOT NULL DEFAULT 0)""")
        # BTC: minutes 0..9 with two ticks each (open=100+i, close=101+i,
        # low/high bracket both); minute 5 missing entirely (gap).
        base = 1_700_000_000_000
        rows = []
        for i in range(10):
            if i == 5:
                continue
            ts = base + i * 60_000
            rows.append((f"b{i}a", "BTC", "coinbase_ws", 100.0 + i, ts, ts))
            rows.append((f"b{i}b", "BTC", "kraken_ws", 101.0 + i, ts + 30_000, ts))
        # ETH: minutes 2..9 (shorter span, offset start; includes minute 5,
        # so the calendar keeps minute 5 and BTC gets an in-span NaN gap)
        for i in range(2, 10):
            ts = base + i * 60_000
            rows.append((f"e{i}", "ETH", "coinbase_ws", 2000.0 + i, ts, ts))
        # decoy source must be excluded
        rows.append(("x0", "BTC", "gemini_ws", 1.0, base, base))
        conn.executemany("INSERT INTO edge_prediction_raw_ticks VALUES (?,?,?,?,?,?)", rows)
        conn.commit()
        conn.close()
        os.environ["OPENTERMINAL_DATA_DB"] = self.db
        self.base = base

    def tearDown(self):
        os.environ.pop("OPENTERMINAL_DATA_DB", None)
        self.tmp.cleanup()

    def _build(self):
        return cd.build({"days": 1, "freq_sec": 60, "out": self.out, "min_ticks": 1,
                         "now_ms": self.base + 11 * 60_000})

    def test_build_layout_and_calendar(self):
        result = self._build()
        self.assertTrue(result["success"], result)
        self.assertEqual(result["freq"], "1min")
        with open(os.path.join(self.out, "calendars", "1min.txt")) as fh:
            cal = [l for l in fh.read().splitlines() if l.strip()]
        # union of observed buckets: BTC 0-9 minus 5, plus ETH 2-9 -> all 10
        self.assertEqual(len(cal), 10)
        self.assertTrue(cal[0].endswith(":00"))
        with open(os.path.join(self.out, "instruments", "all.txt")) as fh:
            lines = [l.split("\t") for l in fh.read().splitlines() if l.strip()]
        self.assertEqual([l[0] for l in lines], ["BTC", "ETH"])

    def test_ohlc_math_and_source_filter(self):
        self._build()
        start, closes = read_bin(os.path.join(self.out, "features", "btc", "close.1min.bin"))
        self.assertEqual(start, 0)                       # BTC starts at calendar[0]
        s2, opens = read_bin(os.path.join(self.out, "features", "btc", "open.1min.bin"))
        self.assertAlmostEqual(opens[0], 100.0, places=4)   # first tick, gemini decoy excluded
        self.assertAlmostEqual(closes[0], 101.0, places=4)  # last tick in bucket
        _, highs = read_bin(os.path.join(self.out, "features", "btc", "high.1min.bin"))
        _, lows = read_bin(os.path.join(self.out, "features", "btc", "low.1min.bin"))
        self.assertAlmostEqual(highs[0], 101.0, places=4)
        self.assertAlmostEqual(lows[0], 100.0, places=4)

    def test_gap_is_nan_and_change_bridges_it(self):
        self._build()
        # minute 5 exists in the calendar (ETH has it) but BTC has no ticks
        # there -> BTC row 5 is NaN, and change at row 6 bridges 4 -> 6.
        _, closes = read_bin(os.path.join(self.out, "features", "btc", "close.1min.bin"))
        self.assertTrue(math.isnan(closes[5]))
        self.assertAlmostEqual(closes[4], 105.0, places=4)
        self.assertAlmostEqual(closes[6], 107.0, places=4)
        _, changes = read_bin(os.path.join(self.out, "features", "btc", "change.1min.bin"))
        self.assertTrue(math.isnan(changes[0]))          # no previous close
        self.assertAlmostEqual(changes[1], 102.0 / 101.0 - 1.0, places=6)
        self.assertAlmostEqual(changes[6], 107.0 / 105.0 - 1.0, places=6)

    def test_eth_start_index_offset(self):
        self._build()
        start, closes = read_bin(os.path.join(self.out, "features", "eth", "close.1min.bin"))
        self.assertEqual(start, 2)                       # ETH begins at calendar row 2
        self.assertEqual(len(closes), 8)                 # rows 2..9, no gaps

    def test_volume_and_vwap_are_all_nan(self):
        # A MISSING field file crashes qlib's expression engine; a NaN-filled
        # one degrades honestly. Both must exist and carry no invented values.
        self._build()
        for field in ("volume", "vwap"):
            _, values = read_bin(os.path.join(self.out, "features", "btc", f"{field}.1min.bin"))
            self.assertTrue(values, field)
            self.assertTrue(all(math.isnan(v) for v in values), field)

    def test_info_reports_dataset(self):
        self._build()
        info = cd.info({"out": self.out})
        self.assertTrue(info["success"])
        self.assertEqual(info["instruments"], ["BTC", "ETH"])
        self.assertEqual(info["freqs"]["1min"]["bars"], 10)

    def test_freq_name(self):
        self.assertEqual(cd.freq_name(60), "1min")
        self.assertEqual(cd.freq_name(300), "5min")
        self.assertEqual(cd.freq_name(86400), "day")


if __name__ == "__main__":
    unittest.main()
