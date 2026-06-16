from trading_mcp.config import Settings
from trading_mcp.safety import RiskManager, TradingBlocked, option_contract_multiplier
from trading_mcp.alpaca_tools import AlpacaService


def expect_block(fn):
    try:
        fn()
    except TradingBlocked:
        return True
    raise AssertionError("expected TradingBlocked")


def main():
    settings = Settings(dry_run=True, trading_mode="paper", max_order_notional_usd=100, enable_alpaca=False, enable_coinbase=False)
    risk = RiskManager(settings)
    report = risk.check_trade(
        venue="alpaca", symbol="AAPL", side="buy", quantity=1,
        estimated_price=99, confirmation_token=None, asset_class="stock"
    )
    assert report["dry_run"] is True
    assert report["estimated_notional_usd"] == 99

    expect_block(lambda: risk.check_trade(
        venue="alpaca", symbol="AAPL", side="buy", quantity=2,
        estimated_price=99, confirmation_token=None, asset_class="stock"
    ))

    assert option_contract_multiplier("AAPL260116C00150000") == 100

    # FAIL-CLOSED: a sized order with no determinable price must be BLOCKED,
    # never allowed to skip the notional cap (the old market-order bypass).
    expect_block(lambda: risk.check_trade(
        venue="alpaca", symbol="AAPL", side="buy", quantity=1,
        estimated_price=None, confirmation_token=None, asset_class="stock"
    ))

    # POSITION CAP: a BUY whose projected position exceeds MAX_POSITION_NOTIONAL_USD is blocked.
    pos_settings = Settings(dry_run=True, trading_mode="paper", max_order_notional_usd=100000,
                            max_position_notional_usd=1000, enable_alpaca=False, enable_coinbase=False)
    pos_risk = RiskManager(pos_settings)
    expect_block(lambda: pos_risk.check_trade(
        venue="alpaca", symbol="AAPL", side="buy", quantity=10, estimated_price=90,  # order 900 < order cap
        current_position_notional=200, confirmation_token=None, asset_class="stock"  # 200 + 900 = 1100 > 1000
    ))
    # A SELL reduces exposure and must NOT be blocked by the position cap.
    sell_report = pos_risk.check_trade(
        venue="alpaca", symbol="AAPL", side="sell", quantity=10, estimated_price=90,
        current_position_notional=2000, confirmation_token=None, asset_class="stock"
    )
    assert sell_report["side"] == "sell"

    bars = [{"close": 10+i} for i in range(40)]
    bt = AlpacaService.backtest_strategy_from_bars(bars, short_window=3, long_window=8)
    assert bt["ok"] is True
    assert "return_pct" in bt

    # Fees reduce returns: same bars with a per-side fee must finish lower.
    bt_fee = AlpacaService.backtest_strategy_from_bars(bars, short_window=3, long_window=8, fee_bps=100)
    assert bt_fee["ok"] is True
    assert bt_fee["return_pct"] < bt["return_pct"]

    # Mean-reversion backtest: runs on an oscillating series; fees never increase return.
    import math
    osc = [{"close": 100 + 10 * math.sin(i / 3.0)} for i in range(80)]
    mr = AlpacaService.backtest_mean_reversion_from_bars(osc, window=20, k=1.0)
    mr_fee = AlpacaService.backtest_mean_reversion_from_bars(osc, window=20, k=1.0, fee_bps=50)
    assert mr["ok"] is True and "return_pct" in mr
    assert mr_fee["return_pct"] <= mr["return_pct"]

    live = Settings(dry_run=False, trading_mode="live", enable_alpaca=False, enable_coinbase=False)
    live_risk = RiskManager(live)
    expect_block(lambda: live_risk.check_trade(
        venue="coinbase", symbol="BTC-USD", side="buy", quantity=0.01,
        estimated_price=50000, confirmation_token=None, asset_class="crypto"
    ))

    print("smoke tests passed")


if __name__ == "__main__":
    main()
