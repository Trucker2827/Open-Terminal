import hashlib
import importlib.util
import json
import sqlite3
import tempfile
import unittest
import zipfile
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "scripts" / "security" / "public_seed_data.py"
SPEC = importlib.util.spec_from_file_location("public_seed_data", SCRIPT)
seed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(seed)


def make_source(path: Path, source_value="coinbase"):
    con = sqlite3.connect(path)
    con.executescript("""
        CREATE TABLE edge_prediction_raw_ticks(
          id TEXT, symbol TEXT NOT NULL, source TEXT NOT NULL, price REAL NOT NULL,
          exchange_ts INTEGER NOT NULL, received_ts INTEGER NOT NULL);
        CREATE TABLE edge_prediction_market_snapshots(
          id TEXT, venue TEXT NOT NULL, symbol TEXT NOT NULL, horizon TEXT NOT NULL,
          market_id TEXT NOT NULL, question TEXT NOT NULL, yes_price REAL NOT NULL,
          no_price REAL NOT NULL, spread_cost REAL NOT NULL, liquidity_score REAL NOT NULL,
          seconds_left INTEGER NOT NULL, observed_at INTEGER NOT NULL);
        CREATE TABLE market_data(
          symbol TEXT, exchange TEXT, interval TEXT, timestamp_ms INTEGER,
          open REAL, high REAL, low REAL, close REAL, volume REAL, oi REAL);
        CREATE TABLE kalshi_live_orders(account_id TEXT, api_key TEXT, request_headers TEXT);
    """)
    con.execute("INSERT INTO edge_prediction_raw_ticks VALUES(?,?,?,?,?,?)",
                ("tick-1", "BTC-USD", source_value, 64000.0, 1000, 1001))
    con.execute("INSERT INTO edge_prediction_market_snapshots VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                ("snap-1", "kalshi", "BTC", "1h", "KXBTC", "BTC above strike?",
                 .51, .50, .01, .8, 3000, 1001))
    con.execute("INSERT INTO market_data VALUES(?,?,?,?,?,?,?,?,?,?)",
                ("BTC-USD", "coinbase", "1h", 1000, 1, 2, .5, 1.5, 10, 0))
    con.execute("INSERT INTO kalshi_live_orders VALUES(?,?,?)",
                ("private-account", "sk_live_never_export", "Authorization: Bearer secret"))
    con.commit()
    con.close()


class PublicSeedDataTest(unittest.TestCase):
    def test_export_copies_only_allowlisted_tables_and_columns(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, bundle = root / "live.db", root / "seed.zip"
            make_source(source)
            result = seed.create_bundle(source, bundle)
            self.assertEqual(set(result["tables"]), set(seed.ALLOWLIST))
            verified = seed.verify_bundle(bundle)
            self.assertTrue(verified["verified"])
            with zipfile.ZipFile(bundle) as archive:
                exported = root / "exported.db"
                exported.write_bytes(archive.read(seed.DATABASE_NAME))
            con = sqlite3.connect(exported)
            tables = {row[0] for row in con.execute(
                "SELECT name FROM sqlite_master WHERE type='table'")}
            con.close()
            self.assertNotIn("kalshi_live_orders", tables)
            self.assertEqual(tables, set(seed.ALLOWLIST))

    def test_unknown_table_is_rejected(self):
        with self.assertRaisesRegex(seed.SeedSecurityError, "forbidden|not explicitly allowlisted"):
            seed.validate_selection(["kalshi_live_orders"])

    def test_new_source_column_requires_explicit_review(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "live.db"
            make_source(source)
            con = sqlite3.connect(source)
            con.execute("ALTER TABLE edge_prediction_raw_ticks ADD COLUMN new_metadata TEXT")
            con.commit()
            con.close()
            with self.assertRaisesRegex(seed.SeedSecurityError, "not explicitly allowlisted"):
                seed.create_bundle(source, root / "seed.zip",
                                   ["edge_prediction_raw_ticks"])

    def test_secret_header_in_allowed_text_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "live.db"
            make_source(source, "Authorization: Bearer abcdefghijklmnop")
            with self.assertRaisesRegex(seed.SeedSecurityError, "secret-like"):
                seed.create_bundle(source, root / "seed.zip",
                                   ["edge_prediction_raw_ticks"])

    def test_local_user_path_in_allowed_text_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "live.db"
            make_source(source, "/Users/alice/private/ticks.csv")
            with self.assertRaisesRegex(seed.SeedSecurityError, "filesystem path"):
                seed.create_bundle(source, root / "seed.zip",
                                   ["edge_prediction_raw_ticks"])

    def test_private_identifier_in_json_is_rejected(self):
        with self.assertRaisesRegex(seed.SeedSecurityError, "JSON key"):
            seed.scan_text('{"client_order_id":"private"}', "test")

    def test_local_username_is_rejected(self):
        import getpass
        with self.assertRaisesRegex(seed.SeedSecurityError, "local username"):
            seed.scan_text(f"collected by {getpass.getuser()}", "test")

    def test_tampered_database_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, bundle = root / "live.db", root / "seed.zip"
            make_source(source)
            seed.create_bundle(source, bundle)
            with zipfile.ZipFile(bundle) as archive:
                manifest = archive.read(seed.MANIFEST_NAME)
            with zipfile.ZipFile(bundle, "w") as archive:
                archive.writestr(seed.DATABASE_NAME, b"not sqlite")
                archive.writestr(seed.MANIFEST_NAME, manifest)
            with self.assertRaisesRegex(seed.SeedSecurityError, "size mismatch|checksum mismatch"):
                seed.verify_bundle(bundle)

    def test_install_is_separate_idempotent_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, bundle, data = root / "live.db", root / "seed.zip", root / "data"
            make_source(source)
            seed.create_bundle(source, bundle)
            first = seed.install_bundle(bundle, data)
            self.assertTrue(first["installed"])
            self.assertEqual(Path(first["database"]).name, seed.DATABASE_NAME)
            self.assertFalse(seed.install_bundle(bundle, data)["installed"])
            (data / seed.DATABASE_NAME).write_bytes(b"user data")
            with self.assertRaisesRegex(seed.SeedSecurityError, "refusing to overwrite"):
                seed.install_bundle(bundle, data)

    def test_stage_produces_only_native_first_launch_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, bundle, staged = root / "live.db", root / "seed.zip", root / "staged"
            make_source(source)
            seed.create_bundle(source, bundle)
            result = seed.stage_bundle(bundle, staged)
            self.assertTrue(result["staged"])
            self.assertEqual({p.name for p in staged.iterdir()},
                             {seed.DATABASE_NAME, seed.MANIFEST_NAME})
            self.assertEqual(seed.sha256_file(staged / seed.DATABASE_NAME),
                             json.loads((staged / seed.MANIFEST_NAME).read_text())[
                                 "database_sha256"])

    def test_download_requires_https_and_pinned_checksum(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "seed.zip"
            with self.assertRaisesRegex(seed.SeedSecurityError, "require HTTPS"):
                seed.download_bundle("http://example.test/seed.zip", output, "0" * 64)
            with self.assertRaisesRegex(seed.SeedSecurityError, "valid expected SHA"):
                seed.download_bundle("https://example.test/seed.zip", output, "bad")


if __name__ == "__main__":
    unittest.main()
