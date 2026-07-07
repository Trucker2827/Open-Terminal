#!/usr/bin/env python3
"""
Trade Intelligence helper for OpenTerminal.

Normalizes public UN Comtrade data into a compact shape that both GUI and CLI
can render: partner rows, summary stats, insight bullets, and market-impact
hints. If the public endpoint is unavailable, it returns a clearly marked
static fallback so the screen remains useful without pretending to be live.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Tuple

import requests


BASE_URL = "https://comtradeapi.un.org/public/v1/preview/C/A/HS"
AUTH_URL = "https://comtradeapi.un.org/data/v1/get/C/A/HS"
TIMEOUT = 18

COUNTRY_CODES = {
    "USA": 842, "CHN": 156, "DEU": 276, "JPN": 392, "GBR": 826,
    "FRA": 251, "TUR": 792, "ITA": 381, "CAN": 124, "KOR": 410,
    "MEX": 484, "BRA": 76, "AUS": 36, "NLD": 528, "CHE": 757,
    "VNM": 704, "SGP": 702, "IRL": 372, "RUS": 643, "ESP": 724,
}

COUNTRY_NAMES = {
    "USA": "United States", "CHN": "China", "DEU": "Germany", "JPN": "Japan",
    "GBR": "United Kingdom", "FRA": "France", "TUR": "Türkiye", "ITA": "Italy",
    "CAN": "Canada", "KOR": "South Korea", "MEX": "Mexico", "BRA": "Brazil",
    "AUS": "Australia", "NLD": "Netherlands", "CHE": "Switzerland",
    "VNM": "Vietnam", "SGP": "Singapore", "IRL": "Ireland",
}

COMMODITIES = {
    "TOTAL": ("All commodities", ["broad market", "shipping", "FX", "industrial cyclicals"]),
    "27": ("Energy / mineral fuels", ["XLE", "oil", "gas", "shipping", "inflation"]),
    "84": ("Machinery", ["industrial equipment", "automation", "capital goods"]),
    "85": ("Electrical machinery / electronics", ["semiconductors", "hardware", "AI supply chain"]),
    "87": ("Vehicles", ["autos", "EVs", "parts suppliers", "freight"]),
    "88": ("Aircraft", ["aerospace", "defense", "airlines"]),
    "90": ("Medical / optical instruments", ["medtech", "healthcare equipment"]),
    "30": ("Pharmaceuticals", ["pharma", "healthcare", "biotech supply chain"]),
    "10": ("Cereals", ["agriculture", "food inflation", "fertilizer"]),
    "72": ("Iron and steel", ["steel", "construction", "infrastructure"]),
}

STATIC_PARTNERS = [
    ("Mexico", "MEX", 437898.0, 345098.0),
    ("Canada", "CAN", 406282.0, 297329.0),
    ("China", "CHN", 427230.0, 177093.0),
    ("Germany", "DEU", 145632.0, 90342.5),
    ("Japan", "JPN", 138420.0, 94699.0),
    ("South Korea", "KOR", 115210.0, 88830.2),
    ("Vietnam", "VNM", 109450.0, 42081.8),
    ("United Kingdom", "GBR", 67342.0, 73428.6),
    ("Türkiye", "TUR", 87120.0, 45012.1),
    ("Ireland", "IRL", 82340.0, 44515.1),
    ("Netherlands", "NLD", 56120.0, 52126.2),
    ("France", "FRA", 62340.0, 45550.2),
    ("Italy", "ITA", 67230.0, 37358.7),
    ("Singapore", "SGP", 48120.0, 51015.2),
    ("Switzerland", "CHE", 54230.0, 38722.6),
]


@dataclass
class Partner:
    name: str
    iso3: str
    imports_m: float = 0.0
    exports_m: float = 0.0
    prev_imports_m: float = 0.0
    prev_exports_m: float = 0.0

    @property
    def total_m(self) -> float:
        return self.imports_m + self.exports_m

    @property
    def balance_m(self) -> float:
        return self.exports_m - self.imports_m

    @property
    def previous_total_m(self) -> float:
        return self.prev_imports_m + self.prev_exports_m

    @property
    def yoy_pct(self) -> Optional[float]:
        prev = self.previous_total_m
        if prev <= 0:
            return None
        return ((self.total_m - prev) / prev) * 100.0


def _clean_iso(value: str) -> str:
    return (value or "").strip().upper()


def _is_india_partner(iso: str, name: str) -> bool:
    if _clean_iso(iso) == "IND":
        return True
    return (name or "").strip().casefold() == "india"


def _normalize_partner_label(iso: str, name: str) -> tuple[str, str]:
    iso = _clean_iso(iso)
    label = (name or iso).strip()
    if iso == "TUR" or label.casefold() in {"turkey", "türkiye", "turkiye"}:
        return "TUR", "Türkiye"
    return iso, label or iso


def _drop_india_partners(partners: Dict[str, Partner]) -> None:
    for iso in list(partners.keys()):
        p = partners[iso]
        if _is_india_partner(p.iso3, p.name):
            del partners[iso]


def _turkiye_partner(rows: Iterable[Partner]) -> Optional[Partner]:
    for partner in rows:
        if _clean_iso(partner.iso3) == "TUR":
            return partner
    for partner in _static_partners():
        if partner.iso3 == "TUR":
            return partner
    return None


def _ensure_turkiye_visible(rows: List[Partner], limit: int) -> List[Partner]:
    capped = rows[: max(1, limit)]
    if any(_clean_iso(p.iso3) == "TUR" for p in capped):
        return capped
    turkiye = _turkiye_partner(rows)
    if turkiye is None:
        return capped
    if len(capped) >= limit:
        capped[-1] = turkiye
    else:
        capped.append(turkiye)
    return capped


def _reporter_code(iso3: str) -> Optional[int]:
    iso = _clean_iso(iso3)
    if iso.isdigit():
        return int(iso)
    return COUNTRY_CODES.get(iso)


def _safe_float(value: Any) -> float:
    try:
        if value is None:
            return 0.0
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _request_flow(reporter: str, year: int, flow_code: str, commodity: str, max_records: int) -> List[Dict[str, Any]]:
    code = _reporter_code(reporter)
    if code is None:
        raise ValueError(f"Unknown reporter ISO/code: {reporter}")

    key = os.environ.get("UN_COMTRADE_API_KEY", "").strip()
    url = AUTH_URL if key else BASE_URL
    params = {
        "reporterCode": code,
        "period": str(year),
        "cmdCode": commodity or "TOTAL",
        "flowCode": flow_code,
        "maxRecords": min(max_records, 100000 if key else 500),
        "includeDesc": "true",
        "breakdownMode": "classic",
    }
    headers = {
        "Accept": "application/json",
        "User-Agent": "OpenTerminal/0.3 trade-intelligence",
    }
    if key:
        headers["Ocp-Apim-Subscription-Key"] = key

    resp = requests.get(url, params=params, headers=headers, timeout=TIMEOUT)
    resp.raise_for_status()
    payload = resp.json()
    if not isinstance(payload, dict) or payload.get("success") is False:
        raise RuntimeError(payload.get("error") or "UN Comtrade returned failure")
    data = payload.get("data", [])
    if not isinstance(data, list):
        return []
    return data


def _merge_records(records: Iterable[Dict[str, Any]], partners: Dict[str, Partner], attr: str) -> None:
    for row in records:
        iso_raw = _clean_iso(str(row.get("partnerISO", "")))
        name_raw = str(row.get("partnerDesc") or row.get("partnerISO") or iso_raw)
        if not iso_raw or iso_raw in {"W00", "0", "WORLD"}:
            continue
        if _is_india_partner(iso_raw, name_raw):
            continue
        iso, name = _normalize_partner_label(iso_raw, name_raw)
        value = _safe_float(row.get("primaryValue"))
        if value <= 0:
            value = _safe_float(row.get("fobvalue")) or _safe_float(row.get("cifvalue"))
        value_m = value / 1_000_000.0
        p = partners.get(iso)
        if p is None:
            p = Partner(name=name, iso3=iso)
            partners[iso] = p
        setattr(p, attr, getattr(p, attr) + value_m)


def _static_partners() -> List[Partner]:
    return [Partner(name=n, iso3=iso, imports_m=imp, exports_m=exp) for n, iso, imp, exp in STATIC_PARTNERS]


def fetch_partners(reporter: str, year: int, commodity: str, max_records: int) -> Tuple[List[Partner], str, str]:
    partners: Dict[str, Partner] = {}
    prev_year = year - 1
    _merge_records(_request_flow(reporter, year, "M", commodity, max_records), partners, "imports_m")
    _merge_records(_request_flow(reporter, year, "X", commodity, max_records), partners, "exports_m")
    try:
        _merge_records(_request_flow(reporter, prev_year, "M", commodity, max_records), partners, "prev_imports_m")
        _merge_records(_request_flow(reporter, prev_year, "X", commodity, max_records), partners, "prev_exports_m")
    except Exception:
        # Current-year live data is still useful; YoY simply remains unavailable.
        pass
    _drop_india_partners(partners)
    rows = [p for p in partners.values() if p.total_m > 0]
    if not rows:
        raise RuntimeError("No partner rows returned")
    rows.sort(key=lambda p: p.total_m, reverse=True)
    return rows, "LIVE", "UN Comtrade public API"


def _market_hints(commodity: str, top: Optional[Partner], largest_deficit: Optional[Partner]) -> List[str]:
    label, assets = COMMODITIES.get(commodity.upper(), (f"HS {commodity}", ["sector ETF", "supply chain"]))
    hints = [
        f"Commodity lens: {label}; watch {', '.join(assets[:4])}.",
    ]
    if top:
        hints.append(f"Largest route by total flow is {top.name}; changes here can move logistics, FX, and exposed multinationals.")
    if largest_deficit and largest_deficit.balance_m < 0:
        hints.append(f"Biggest deficit route is {largest_deficit.name}; useful for tariff, reshoring, and supply-chain monitoring.")
    if commodity.upper() in {"84", "85", "90"}:
        hints.append("Tech/manufacturing chapters can matter for semis, AI hardware, industrial automation, and inventory cycles.")
    elif commodity.upper() == "27":
        hints.append("Energy flows can connect to crude, LNG, inflation expectations, tanker rates, and energy equities.")
    elif commodity.upper() == "87":
        hints.append("Vehicle flows can connect to autos, EV credits, parts suppliers, rail/trucking, and tariff headlines.")
    return hints


def _insights(rows: List[Partner], reporter: str, year: int, commodity: str, status: str) -> List[str]:
    if not rows:
        return ["No trade-flow rows available."]
    top = rows[0]
    largest_import = max(rows, key=lambda p: p.imports_m)
    largest_export = max(rows, key=lambda p: p.exports_m)
    largest_deficit = min(rows, key=lambda p: p.balance_m)
    largest_surplus = max(rows, key=lambda p: p.balance_m)
    rising = [p for p in rows if p.yoy_pct is not None]
    rising.sort(key=lambda p: p.yoy_pct or -999, reverse=True)

    surplus_text = f"{largest_surplus.name} (+${largest_surplus.balance_m:,.0f}M)"
    if largest_surplus.balance_m <= 0:
        surplus_text = f"no surplus in displayed set; least-negative is {largest_surplus.name} (${largest_surplus.balance_m:,.0f}M)"

    bullets = [
        f"{status}: {reporter.upper()} {year} trade map loaded for {COMMODITIES.get(commodity.upper(), (commodity, []))[0]}.",
        f"Top total partner: {top.name} (${top.total_m:,.0f}M).",
        f"Largest import route: {largest_import.name} (${largest_import.imports_m:,.0f}M).",
        f"Largest export route: {largest_export.name} (${largest_export.exports_m:,.0f}M).",
        f"Biggest deficit/surplus: {largest_deficit.name} (${largest_deficit.balance_m:,.0f}M), {surplus_text}.",
    ]
    if rising and rising[0].yoy_pct is not None:
        bullets.append(f"Fastest YoY riser in top set: {rising[0].name} ({rising[0].yoy_pct:+.1f}%).")
    bullets.extend(_market_hints(commodity, top, largest_deficit))
    return bullets[:8]


def _row_json(partner: Partner, rank: int) -> Dict[str, Any]:
    yoy = partner.yoy_pct
    return {
        "rank": rank,
        "partner": partner.name,
        "iso3": partner.iso3,
        "imports_m": round(partner.imports_m, 3),
        "exports_m": round(partner.exports_m, 3),
        "total_m": round(partner.total_m, 3),
        "balance_m": round(partner.balance_m, 3),
        "yoy_pct": None if yoy is None else round(yoy, 3),
    }


def build_flow(reporter: str, year: int, commodity: str, limit: int, max_records: int) -> Dict[str, Any]:
    started = time.time()
    status = "LIVE"
    source = "UN Comtrade public API"
    error = ""
    try:
        rows, status, source = fetch_partners(reporter, year, commodity, max_records)
    except Exception as exc:
        status = "FALLBACK"
        source = "static bundled sample"
        error = str(exc)
        rows = _static_partners()

    rows = [p for p in rows if not _is_india_partner(p.iso3, p.name)]
    rows.sort(key=lambda p: p.total_m, reverse=True)
    rows = _ensure_turkiye_visible(rows, limit)
    total_imports = sum(p.imports_m for p in rows)
    total_exports = sum(p.exports_m for p in rows)
    out = {
        "success": True,
        "status": status,
        "source": source,
        "source_url": "https://comtrade.un.org/",
        "reporter": reporter.upper(),
        "reporter_name": COUNTRY_NAMES.get(reporter.upper(), reporter.upper()),
        "year": year,
        "commodity": commodity.upper(),
        "commodity_name": COMMODITIES.get(commodity.upper(), (commodity.upper(), []))[0],
        "generated_at": int(time.time()),
        "latency_ms": int((time.time() - started) * 1000),
        "error": error,
        "summary": {
            "partners": len(rows),
            "imports_m": round(total_imports, 3),
            "exports_m": round(total_exports, 3),
            "total_m": round(total_imports + total_exports, 3),
            "balance_m": round(total_exports - total_imports, 3),
        },
        "partners": [_row_json(p, i + 1) for i, p in enumerate(rows)],
        "insights": _insights(rows, reporter, year, commodity, status),
    }
    return out


def print_table(payload: Dict[str, Any]) -> None:
    print(f"{payload['status']} {payload['reporter']} {payload['year']} {payload['commodity_name']}")
    if payload.get("error"):
        print(f"fallback reason: {payload['error']}")
    print(f"source: {payload['source']}")
    print()
    print(f"{'R':>2} {'PARTNER':<18} {'IMPORTS $M':>12} {'EXPORTS $M':>12} {'BALANCE $M':>12} {'YOY':>8}")
    for row in payload.get("partners", []):
        yoy = row.get("yoy_pct")
        yoy_s = "--" if yoy is None else f"{yoy:+.1f}%"
        print(f"{row['rank']:>2} {row['partner'][:18]:<18} {row['imports_m']:>12,.0f} {row['exports_m']:>12,.0f} {row['balance_m']:>12,.0f} {yoy_s:>8}")
    print()
    print("INSIGHTS")
    for item in payload.get("insights", []):
        print(f"- {item}")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="OpenTerminal trade intelligence")
    sub = parser.add_subparsers(dest="command")

    flow = sub.add_parser("flow", help="Fetch normalized country trade-flow intelligence")
    flow.add_argument("--reporter", default="USA", help="Reporter ISO3 or UN numeric code")
    flow.add_argument("--year", type=int, default=2024)
    flow.add_argument("--commodity", default="TOTAL", help="HS code or TOTAL")
    flow.add_argument("--limit", type=int, default=15)
    flow.add_argument("--max-records", type=int, default=500)
    flow.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)
    if args.command in {None, "flow"}:
        payload = build_flow(args.reporter, args.year, args.commodity, args.limit, args.max_records)
        if getattr(args, "json", False):
            print(json.dumps(payload, separators=(",", ":")))
        else:
            print_table(payload)
        return 0

    parser.print_help(sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
