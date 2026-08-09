#!/usr/bin/env python3
"""Kalshi BTC daily/band floor calibrator (series KXBTC).

Models floor-only "or above" contracts; skips range/cap books. Own trust:
kxbtc-daily-calibrator.json. Not KXBTCD (hourly threshold) and not KXBTC15M.
"""
from __future__ import annotations

import sys

import strike_threshold_family as stf


def main(argv=None):
    return stf.main_for(stf.KXBTC_DAILY, argv or sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
