from __future__ import annotations

from functools import lru_cache
from typing import Literal
from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    trading_mode: Literal["paper", "live"] = "paper"
    dry_run: bool = True
    require_confirmation: bool = True
    confirmation_token: str = "I_UNDERSTAND_LIVE_TRADE"
    rate_limit_per_min: int = Field(default=30, ge=1, le=300)
    log_level: str = "INFO"

    max_order_notional_usd: float = Field(default=500.0, gt=0)
    max_position_notional_usd: float = Field(default=2500.0, gt=0)
    max_daily_order_count: int = Field(default=25, ge=0)
    allow_options_trading: bool = False
    allow_crypto_withdrawals: bool = False

    enable_alpaca: bool = True
    alpaca_api_key: str | None = None
    alpaca_secret_key: str | None = None
    alpaca_paper: bool = True
    alpaca_base_url: str | None = None

    enable_coinbase: bool = True
    coinbase_api_key: str | None = None
    coinbase_api_secret: str | None = None
    # Separate execution arm for Coinbase (real-money; no paper mode). Reads/market
    # data/dry-run previews always work; a REAL Coinbase order requires DRY_RUN=false
    # AND this armed. Default off — observe/simulate by default, execute only when
    # deliberately armed for a session.
    coinbase_allow_trading: bool = False
    # Comma-separated symbols allowed to EXECUTE real Coinbase orders when armed.
    # Fail-closed: empty = nothing may execute (NOT "allow everything").
    coinbase_allowed_symbols: str = ""

    enable_kraken: bool = False
    kraken_api_key: str | None = None
    kraken_api_secret: str | None = None
    # Kraken spot account is real money. Reads + dry-run previews work; a REAL
    # order requires DRY_RUN=false AND KRAKEN_ALLOW_TRADING=true. Kraken orders
    # are forced to spot limit post-only in KrakenService.
    kraken_allow_trading: bool = False
    # Accepts Kraken pairs or common product ids: XBTUSD, BTC-USD, ETH-USD, ...
    # Normalized to exchange pair form and fail-closed when empty.
    kraken_allowed_symbols: str = ""

    @field_validator("coinbase_api_secret")
    @classmethod
    def normalize_multiline_secret(cls, value: str | None) -> str | None:
        if value:
            return value.replace("\\n", "\n")
        return value

    def coinbase_symbol_allowlist(self) -> set[str]:
        """Normalized (uppercase, stripped) set of symbols allowed for live
        Coinbase execution. Empty set => nothing allowed (fail-closed)."""
        return {s.strip().upper() for s in self.coinbase_allowed_symbols.split(",") if s.strip()}

    def kraken_symbol_allowlist(self) -> set[str]:
        """Normalized Kraken pair allowlist. Empty set => nothing can execute."""
        aliases = {
            "BTCUSD": "XBTUSD",
            "BTC-USD": "XBTUSD",
            "BTC/USD": "XBTUSD",
            "XBT-USD": "XBTUSD",
            "XBT/USD": "XBTUSD",
            "ETH-USD": "ETHUSD",
            "ETH/USD": "ETHUSD",
            "SOL-USD": "SOLUSD",
            "SOL/USD": "SOLUSD",
            "DOGE-USD": "DOGEUSD",
            "DOGE/USD": "DOGEUSD",
        }
        out = set()
        for symbol in self.kraken_allowed_symbols.split(","):
            key = symbol.strip().upper()
            if not key:
                continue
            compact = key.replace("-", "").replace("/", "")
            out.add(aliases.get(key, aliases.get(compact, compact)))
        return out


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    return Settings()
