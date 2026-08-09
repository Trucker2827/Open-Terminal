#!/usr/bin/env python3
"""Kalshi commodities daily strike calibrator (KXGOLDD / KXSILVERD / KXWTI).

Threshold "above $Y" books. WTI daily series ticker is KXWTI. Own trust file:
commodities-daily-calibrator.json. See strike_threshold_family.py.
"""
from __future__ import annotations

import sys

import strike_threshold_family as stf


def main(argv=None):
    return stf.main_for(stf.COMMODITIES_DAILY, argv or sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
