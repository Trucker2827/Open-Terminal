#!/usr/bin/env python3
"""
Fast-Trade <-> OpenMarketTerminal Strategy Bridge
Executes OpenMarketTerminal Engine strategies within Fast-Trade provider
"""

import sys
import json
from pathlib import Path

# Add base directory to path
BASE_DIR = Path(__file__).resolve().parent.parent / "base"
sys.path.insert(0, str(BASE_DIR))

from openmarketterminal_strategy_runner import OpenMarketTerminalStrategyRunner


def run_openmarketterminal_strategy(strategy_id: str, params_dict: dict) -> dict:
    """
    Execute OpenMarketTerminal strategy and return Fast-Trade-compatible results.

    Args:
        strategy_id: OpenMarketTerminal strategy ID (FCT-XXXXXXXX)
        params_dict: Backtest parameters

    Returns:
        dict: Fast-Trade-format results
    """
    runner = OpenMarketTerminalStrategyRunner()
    result = runner.execute_strategy(strategy_id, params_dict)

    if not result.get('success'):
        return result

    # Convert to Fast-Trade format (already compatible)
    return result


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(json.dumps({
            'success': False,
            'error': 'Usage: ft_openmarketterminal_bridge.py <strategy_id> <params_json>'
        }))
        sys.exit(1)

    strategy_id = sys.argv[1]
    params = json.loads(sys.argv[2])

    result = run_openmarketterminal_strategy(strategy_id, params)
    print(json.dumps(result, indent=2))
