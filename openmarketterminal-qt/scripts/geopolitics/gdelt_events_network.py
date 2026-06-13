"""Build a real actor→actor geopolitical relationship network from GDELT.

GDELT 2.0 publishes a CAMEO-coded event export every 15 minutes. Each event
carries Actor1/Actor2 country codes, an event code, a GoldsteinScale
(-10 conflict … +10 cooperation), and a mention count. We aggregate the last
few exports into directed country→country edges — no NLP on our side; GDELT
already coded the events.

Usage:  python gdelt_events_network.py [--files N] [--top N]
Output (stdout JSON):
  {"ok": true, "window": "...", "actors": <int>, "relationships": <int>,
   "edges": [{"from_code","from","to_code","to","count","goldstein","mentions",
              "polarity": "conflict|cooperation"}], ...}
Failure: {"ok": false, "error": "..."}
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import sys
import urllib.request
import zipfile
from collections import defaultdict
from datetime import datetime, timedelta

LASTUPDATE = "http://data.gdeltproject.org/gdeltv2/lastupdate.txt"
EXPORT_FMT = "http://data.gdeltproject.org/gdeltv2/%s.export.CSV.zip"
UA = {"User-Agent": "OpenTerminal/0.1.0 (https://github.com/Trucker2827/Open-Terminal)"}

# GDELT 2.0 export column indices (0-based; verified against the live schema).
C_A1_COUNTRY = 7
C_A2_COUNTRY = 17
C_EVENT_ROOT = 28
C_GOLDSTEIN = 30
C_NUM_MENTIONS = 31

# CAMEO/ISO-3 country code → display name. Unmapped codes fall back to the raw
# code, so the data stays honest (real GDELT codes) even without a name.
COUNTRY = {
    "USA": "United States", "RUS": "Russia", "CHN": "China", "GBR": "Britain",
    "FRA": "France", "DEU": "Germany", "ISR": "Israel", "PSE": "Palestine",
    "IRN": "Iran", "IRQ": "Iraq", "SYR": "Syria", "YEM": "Yemen", "SAU": "Saudi Arabia",
    "UKR": "Ukraine", "IND": "India", "PAK": "Pakistan", "AFG": "Afghanistan",
    "JPN": "Japan", "KOR": "South Korea", "PRK": "North Korea", "TWN": "Taiwan",
    "TUR": "Turkey", "EGY": "Egypt", "LBN": "Lebanon", "JOR": "Jordan", "QAT": "Qatar",
    "ARE": "UAE", "ITA": "Italy", "ESP": "Spain", "POL": "Poland", "CAN": "Canada",
    "MEX": "Mexico", "BRA": "Brazil", "ARG": "Argentina", "AUS": "Australia",
    "ZAF": "South Africa", "NGA": "Nigeria", "ETH": "Ethiopia", "SDN": "Sudan",
    "COD": "DR Congo", "LBY": "Libya", "VEN": "Venezuela", "COL": "Colombia",
    "CUB": "Cuba", "LTU": "Lithuania", "LVA": "Latvia", "EST": "Estonia",
    "FIN": "Finland", "SWE": "Sweden", "NOR": "Norway", "NLD": "Netherlands",
    "BEL": "Belgium", "CHE": "Switzerland", "AUT": "Austria", "GRC": "Greece",
    "ROU": "Romania", "HUN": "Hungary", "CZE": "Czechia", "BGR": "Bulgaria",
    "SRB": "Serbia", "HRV": "Croatia", "BLR": "Belarus", "GEO": "Georgia",
    "ARM": "Armenia", "AZE": "Azerbaijan", "KAZ": "Kazakhstan", "MYS": "Malaysia",
    "IDN": "Indonesia", "PHL": "Philippines", "THA": "Thailand", "VNM": "Vietnam",
    "SGP": "Singapore", "NZL": "New Zealand", "MMR": "Myanmar", "BGD": "Bangladesh",
    "LKA": "Sri Lanka", "NPL": "Nepal", "KEN": "Kenya", "SOM": "Somalia",
    "MLI": "Mali", "NER": "Niger", "TCD": "Chad", "CMR": "Cameroon", "GHA": "Ghana",
    "IRL": "Ireland", "DNK": "Denmark", "PRT": "Portugal", "SVK": "Slovakia",
    "SVN": "Slovenia", "ISL": "Iceland", "LUX": "Luxembourg", "MAR": "Morocco",
    "DZA": "Algeria", "TUN": "Tunisia", "KWT": "Kuwait", "BHR": "Bahrain",
    "OMN": "Oman", "UZB": "Uzbekistan", "MDA": "Moldova", "CYP": "Cyprus",
}


def _name(code: str) -> str:
    return COUNTRY.get(code, code)


def _recent_export_urls(n: int) -> list[str]:
    """Latest export URL from lastupdate, plus the prior (n-1) 15-min slots."""
    lu = urllib.request.urlopen(
        urllib.request.Request(LASTUPDATE, headers=UA), timeout=15).read().decode()
    latest = next(l.split()[2] for l in lu.strip().splitlines()
                  if "export.CSV.zip" in l)
    stamp = latest.rsplit("/", 1)[1].split(".")[0]          # YYYYMMDDHHMMSS
    t0 = datetime.strptime(stamp, "%Y%m%d%H%M%S")
    return [EXPORT_FMT % (t0 - timedelta(minutes=15 * i)).strftime("%Y%m%d%H%M%S")
            for i in range(max(1, n))]


def _read_export(url: str) -> list[list[str]]:
    raw = urllib.request.urlopen(
        urllib.request.Request(url, headers=UA), timeout=25).read()
    zf = zipfile.ZipFile(io.BytesIO(raw))
    data = zf.read(zf.namelist()[0]).decode("utf-8", "replace")
    return list(csv.reader(io.StringIO(data), delimiter="\t"))


def build(files: int, top: int) -> dict:
    urls = _recent_export_urls(files)
    # edge key (a1, a2) -> [count, goldstein_sum, mentions_sum]
    edges: dict[tuple, list] = defaultdict(lambda: [0, 0.0, 0])
    actor_vol: dict[str, int] = defaultdict(int)
    parsed = 0
    for url in urls:
        try:
            rows = _read_export(url)
        except Exception:
            continue                                        # skip a missing slot
        for r in rows:
            if len(r) <= C_NUM_MENTIONS:
                continue
            a1, a2 = r[C_A1_COUNTRY].strip(), r[C_A2_COUNTRY].strip()
            if not a1 or not a2 or a1 == a2:
                continue
            try:
                g = float(r[C_GOLDSTEIN] or 0.0)
                m = int(r[C_NUM_MENTIONS] or 0)
            except ValueError:
                continue
            e = edges[(a1, a2)]
            e[0] += 1
            e[1] += g
            e[2] += m
            actor_vol[a1] += m
            actor_vol[a2] += m
            parsed += 1

    if not edges:
        return {"ok": False, "error": "No country-to-country events in the latest GDELT exports."}

    ranked = sorted(edges.items(), key=lambda kv: kv[1][2], reverse=True)[:top]
    out_edges = []
    actors = set()
    for (a1, a2), (cnt, gsum, msum) in ranked:
        g = gsum / cnt if cnt else 0.0
        actors.add(a1)
        actors.add(a2)
        out_edges.append({
            "from_code": a1, "from": _name(a1),
            "to_code": a2, "to": _name(a2),
            "count": cnt,
            "goldstein": round(g, 2),
            "mentions": msum,
            "polarity": "conflict" if g < 0 else "cooperation",
        })
    return {
        "ok": True,
        "window": f"last {len(urls)} GDELT exports (~{len(urls) * 15} min)",
        "events_scanned": parsed,
        "actors": len(actors),
        "relationships": len(out_edges),
        "edges": out_edges,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", type=int, default=4)   # ~1 hour of events
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    try:
        print(json.dumps(build(a.files, a.top)))
    except Exception as e:
        print(json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"}))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
