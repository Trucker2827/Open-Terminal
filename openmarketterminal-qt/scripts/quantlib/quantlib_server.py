"""Local QuantLib REST server — real quantitative-finance computation.

The app's QuantLib screen is a thin HTTP client that POSTs to
  <connectors.quantlib_url>/quantlib/<endpoint>
This server answers those requests with REAL results from the QuantLib library
(option pricing, Greeks, implied vol, binomial) and scipy (distributions).
Endpoints it doesn't implement return an honest {"implemented": false, ...} —
never a fabricated number.

Run:  python quantlib_server.py --port 8800
"""
from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

try:
    import QuantLib as ql
    QL_OK = True
except Exception:
    QL_OK = False

try:
    import scipy.stats as _st
    SCIPY_OK = True
except Exception:
    SCIPY_OK = False


# ── option-pricing helpers (real QuantLib) ──────────────────────────────────

def _bs_option(body, engine="analytic"):
    if not QL_OK:
        raise RuntimeError("QuantLib library not available on the server.")
    spot = float(body["spot"]); strike = float(body["strike"])
    rate = float(body.get("rate", 0.0)); div = float(body.get("dividend", 0.0))
    vol = float(body["vol"]); mat = float(body["maturity"])
    otype = ql.Option.Call if str(body.get("type", "call")).lower().startswith("c") else ql.Option.Put

    today = ql.Date.todaysDate()
    ql.Settings.instance().evaluationDate = today
    dc = ql.Actual365Fixed()
    q_spot = ql.QuoteHandle(ql.SimpleQuote(spot))
    rTS = ql.YieldTermStructureHandle(ql.FlatForward(today, rate, dc))
    qTS = ql.YieldTermStructureHandle(ql.FlatForward(today, div, dc))
    volTS = ql.BlackVolTermStructureHandle(ql.BlackConstantVol(today, ql.NullCalendar(), vol, dc))
    proc = ql.BlackScholesMertonProcess(q_spot, qTS, rTS, volTS)
    exp = today + ql.Period(int(round(mat * 365)), ql.Days)
    opt = ql.VanillaOption(ql.PlainVanillaPayoff(otype, strike), ql.EuropeanExercise(exp))
    if engine == "binomial-eu":
        opt.setPricingEngine(ql.BinomialVanillaEngine(proc, "crr", int(body.get("steps", 200))))
    elif engine == "binomial-am":
        opt = ql.VanillaOption(ql.PlainVanillaPayoff(otype, strike), ql.AmericanExercise(today, exp))
        opt.setPricingEngine(ql.BinomialVanillaEngine(proc, "crr", int(body.get("steps", 200))))
    else:
        opt.setPricingEngine(ql.AnalyticEuropeanEngine(proc))
    return opt


def ep_bs_price(body):
    return {"price": round(_bs_option(body).NPV(), 6)}


def ep_bs_greeks(body):
    o = _bs_option(body)
    return {"price": round(o.NPV(), 6), "delta": round(o.delta(), 6), "gamma": round(o.gamma(), 6),
            "vega": round(o.vega(), 6), "theta": round(o.theta(), 6), "rho": round(o.rho(), 6)}


def ep_bs_implied_vol(body):
    if not QL_OK:
        raise RuntimeError("QuantLib library not available on the server.")
    target = float(body["price"])
    b = dict(body); b.setdefault("vol", 0.2)
    o = _bs_option(b)
    today = ql.Date.todaysDate()
    dc = ql.Actual365Fixed()
    q_spot = ql.QuoteHandle(ql.SimpleQuote(float(body["spot"])))
    rTS = ql.YieldTermStructureHandle(ql.FlatForward(today, float(body.get("rate", 0.0)), dc))
    qTS = ql.YieldTermStructureHandle(ql.FlatForward(today, float(body.get("dividend", 0.0)), dc))
    volTS = ql.BlackVolTermStructureHandle(ql.BlackConstantVol(today, ql.NullCalendar(), 0.2, dc))
    proc = ql.BlackScholesMertonProcess(q_spot, qTS, rTS, volTS)
    iv = o.impliedVolatility(target, proc)
    return {"implied_vol": round(iv, 6)}


def ep_binomial_european(body):
    return {"price": round(_bs_option(body, engine="binomial-eu").NPV(), 6)}


def ep_binomial_american(body):
    return {"price": round(_bs_option(body, engine="binomial-am").NPV(), 6)}


# ── distributions (scipy) ───────────────────────────────────────────────────

def _dist(name, body):
    if not SCIPY_OK:
        raise RuntimeError("scipy not available on the server.")
    fam, fn = name.split("/")[2], name.split("/")[3]
    if fam == "normal":
        d = _st.norm()
    elif fam == "t":
        d = _st.t(float(body["df"]))
    elif fam == "chi2":
        d = _st.chi2(float(body["df"]))
    else:
        raise KeyError(fam)
    if fn == "cdf":
        return {"cdf": round(float(d.cdf(float(body["x"]))), 8)}
    if fn == "pdf":
        return {"pdf": round(float(d.pdf(float(body["x"]))), 8)}
    if fn == "ppf":
        return {"ppf": round(float(d.ppf(float(body.get("p", body.get("x"))))), 8)}
    raise KeyError(fn)


# ── conventions ─────────────────────────────────────────────────────────────

def ep_days_to_years(body):
    return {"years": round(float(body["days"]) / 365.25, 8)}


def ep_years_to_days(body):
    return {"days": round(float(body["years"]) * 365.25, 4)}


# ── GET reference data ──────────────────────────────────────────────────────

def ep_currencies(_):
    return {"currencies": ["USD", "EUR", "GBP", "JPY", "CHF", "CAD", "AUD", "CNY", "INR", "HKD"]}


def ep_daycount_conventions(_):
    return {"conventions": ["Actual360", "Actual365Fixed", "ActualActual", "Thirty360", "Business252"]}


POST_ROUTES = {
    "pricing/bs/price": ep_bs_price,
    "pricing/bs/greeks": ep_bs_greeks,
    "pricing/bs/greeks-full": ep_bs_greeks,
    "pricing/bs/implied-vol": ep_bs_implied_vol,
    "core/ops/black-scholes": ep_bs_price,
    "pricing/binomial/european": ep_binomial_european,
    "pricing/binomial/american": ep_binomial_american,
    "core/conventions/days-to-years": ep_days_to_years,
    "core/conventions/years-to-days": ep_years_to_days,
}
GET_ROUTES = {
    "core/types/currencies": ep_currencies,
    "scheduling/daycount/conventions": ep_daycount_conventions,
}


def dispatch(method, endpoint, body):
    if endpoint.startswith("core/distributions/"):
        return _dist(endpoint, body)
    routes = GET_ROUTES if method == "GET" else POST_ROUTES
    fn = routes.get(endpoint)
    if fn is None:
        return {"implemented": False,
                "error": f"Endpoint '{endpoint}' is not implemented in the local QuantLib engine yet."}
    return fn(body)


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        payload = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *a):  # quiet
        pass

    def _endpoint(self):
        path = urlparse(self.path).path
        return path[len("/quantlib/"):] if path.startswith("/quantlib/") else path.lstrip("/")

    def do_GET(self):
        ep = self._endpoint()
        if ep in ("health", ""):
            return self._send(200, {"status": "ok", "engine": "QuantLib " + (ql.__version__ if QL_OK else "unavailable"),
                                    "quantlib": QL_OK, "scipy": SCIPY_OK})
        try:
            self._send(200, dispatch("GET", ep, {}))
        except Exception as e:
            self._send(200, {"implemented": False, "error": f"{type(e).__name__}: {e}"})

    def do_POST(self):
        ep = self._endpoint()
        try:
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}") if n else {}
        except Exception:
            return self._send(400, {"error": "invalid JSON body"})
        try:
            self._send(200, dispatch("POST", ep, body))
        except KeyError as e:
            self._send(200, {"error": f"missing/invalid field: {e}"})
        except Exception as e:
            self._send(200, {"implemented": False, "error": f"{type(e).__name__}: {e}"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"QuantLib server on http://{a.host}:{a.port}  (QuantLib={QL_OK}, scipy={SCIPY_OK})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
