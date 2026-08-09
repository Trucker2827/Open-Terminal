#!/usr/bin/env python3
"""Kalshi commodities hourly strike calibrator (KXGOLDH / KXSILVERH / KXWTIH).

Threshold "above $Y" books — not 15m open→close races. Own trust file:
commodities-hourly-calibrator.json. See strike_threshold_family.py.
"""
from __future__ import annotations

import sys

import strike_threshold_family as stf


def main(argv=None):
    return stf.main_for(stf.COMMODITIES_HOURLY, argv or sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
