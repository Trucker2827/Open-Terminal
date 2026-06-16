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

    @field_validator("coinbase_api_secret")
    @classmethod
    def normalize_multiline_secret(cls, value: str | None) -> str | None:
        if value:
            return value.replace("\\n", "\n")
        return value


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    return Settings()
