"""Regression tests for the edgartools-backed Form 4 + 13F parsers.

Guards the data contract the UI consumes (review conditions):
  2. Form 4 (AAPL/MSFT) returns owner, transaction code, shares, price.
  3. 13F (BRK-B) returns real issuer rows, not header strings.
  4. Empty parsed rows = failure (error), NOT silent success — unless the filer
     genuinely has no filings.

Network tests hit live SEC EDGAR (skipped automatically if offline). Contract
tests are pure (monkeypatched, no network). Run from the scripts/ dir:
    EDGAR_IDENTITY="you@example.com" python -m mcp.edgar.test_parsers
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))  # scripts/

from mcp.edgar import base, forms_insider, forms_13f  # noqa: E402

_IDENT = os.environ.get("EDGAR_IDENTITY", "OpenTerminal trucker2827@gmail.com")


def _network_ok():
    try:
        base.initialize_edgar(_IDENT)
        from edgar import Company
        return Company("AAPL").get_filings(form="4").latest(1) is not None
    except Exception:
        return False


NET = _network_ok()


class TestForm4Live(unittest.TestCase):
    def _assert_real_rows(self, ticker):
        r = forms_insider.get_insider_transactions(ticker, 25)
        self.assertNotIn("error", r, msg=str(r.get("error")))
        rows = r.get("data") or []
        self.assertTrue(rows, f"{ticker}: no rows returned")
        good = [x for x in rows
                if x.get("insider") and x.get("code")
                and (x.get("shares") or 0) > 0 and (x.get("price") or 0) > 0]
        self.assertTrue(good, f"{ticker}: no row with insider+code+shares+price; got {rows[:2]}")

    @unittest.skipUnless(NET, "no network / SEC unreachable")
    def test_aapl_form4_has_owner_code_shares_price(self):
        self._assert_real_rows("AAPL")

    @unittest.skipUnless(NET, "no network / SEC unreachable")
    def test_msft_form4_has_owner_code_shares_price(self):
        self._assert_real_rows("MSFT")


class TestThirteenFLive(unittest.TestCase):
    @unittest.skipUnless(NET, "no network / SEC unreachable")
    def test_brkb_13f_returns_real_issuer_rows(self):
        r = forms_13f.get_13f_top_holdings("BRK-B", 20)
        self.assertNotIn("error", r, msg=str(r.get("error")))
        h = r["data"]["top_holdings"]
        self.assertGreaterEqual(len(h), 10, "expected many holdings")
        headers = {"issuer", "class", "cusip", "value"}
        self.assertNotIn((h[0].get("issuer") or "").strip().lower(), headers,
                         f"top holding is a header string, not data: {h[0]}")
        self.assertTrue(h[0].get("issuer") and (h[0].get("value") or 0) > 0
                        and (h[0].get("shares") or 0) > 0, f"holding missing fields: {h[0]}")


# ---- pure contract tests (no network) ----
class _Owner:
    def __init__(self, name): self.name = name


def _fake_filing(activities):
    acts = activities

    class _Obj:
        insider_name = "Test Insider"
        position = "Director"
        reporting_owners = [_Owner("Test Insider")]
        def get_transaction_activities(self): return acts

    class _Filing:
        filing_date = "2026-01-01"
        accession_number = "0000000000-00-000000"
        def obj(self): return _Obj()

    return _Filing()


class _FakeFilings(list):
    def latest(self, n=1): return self[0] if self else None


def _fake_company(filings):
    class _C:
        def get_filings(self, form=None): return _FakeFilings(filings)
    return lambda _ticker: _C()


class TestContract(unittest.TestCase):
    def test_filings_with_zero_transactions_is_failure(self):
        orig = forms_insider.Company
        forms_insider.Company = _fake_company([_fake_filing([])])
        try:
            r = forms_insider.get_insider_transactions("AAPL", 25)
            self.assertIn("error", r, "filings present but 0 parsed rows must be an error, not silent success")
        finally:
            forms_insider.Company = orig

    def test_no_filings_is_empty_success(self):
        orig = forms_insider.Company
        forms_insider.Company = _fake_company([])
        try:
            r = forms_insider.get_insider_transactions("ZZZZ", 25)
            self.assertTrue(r.get("success"))
            self.assertEqual(r.get("data"), [])
        finally:
            forms_insider.Company = orig


if __name__ == "__main__":
    print(f"network tests {'ENABLED' if NET else 'SKIPPED (offline)'}")
    unittest.main(verbosity=2)
