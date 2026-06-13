from __future__ import annotations

import asyncio
import json
import time
from dataclasses import dataclass
from typing import Any, Iterable

import websockets

from .auth import KalshiCredentials


PROD_WS_URL = "wss://external-api-ws.kalshi.com/trade-api/ws/v2"
DEMO_WS_URL = "wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2"


@dataclass(frozen=True)
class KalshiWebSocketClient:
    credentials: KalshiCredentials
    env: str = "prod"

    @property
    def url(self) -> str:
        return DEMO_WS_URL if self.env == "demo" else PROD_WS_URL

    async def watch(
        self,
        *,
        market_tickers: Iterable[str],
        channels: Iterable[str],
        seconds: float,
    ) -> list[dict[str, object]]:
        headers = self.credentials.headers("GET", "/trade-api/ws/v2")
        deadline = time.monotonic() + seconds
        messages: list[dict[str, object]] = []

        async with websockets.connect(self.url, additional_headers=headers) as websocket:
            await websocket.send(
                json.dumps(
                    {
                        "id": 1,
                        "cmd": "subscribe",
                        "params": {
                            "channels": list(channels),
                            "market_tickers": list(market_tickers),
                        },
                    }
                )
            )

            while time.monotonic() < deadline:
                timeout = max(0.1, deadline - time.monotonic())
                try:
                    raw = await asyncio.wait_for(websocket.recv(), timeout=timeout)
                except TimeoutError:
                    break
                message = json.loads(raw)
                messages.append(message)
        return messages

    async def stream(
        self,
        *,
        market_tickers: Iterable[str],
        channels: Iterable[str],
    ):
        headers = self.credentials.headers("GET", "/trade-api/ws/v2")
        async with websockets.connect(self.url, additional_headers=headers) as websocket:
            await websocket.send(
                json.dumps(
                    {
                        "id": 1,
                        "cmd": "subscribe",
                        "params": {
                            "channels": list(channels),
                            "market_tickers": list(market_tickers),
                        },
                    }
                )
            )

            async for raw in websocket:
                yield json.loads(raw)

    async def stream_raw(
        self,
        *,
        channels: Iterable[str],
        extra_params: dict[str, Any] | None = None,
    ):
        headers = self.credentials.headers("GET", "/trade-api/ws/v2")
        params: dict[str, Any] = {"channels": list(channels)}
        if extra_params:
            params.update(extra_params)
        async with websockets.connect(self.url, additional_headers=headers) as websocket:
            await websocket.send(
                json.dumps(
                    {
                        "id": 1,
                        "cmd": "subscribe",
                        "params": params,
                    }
                )
            )

            async for raw in websocket:
                yield json.loads(raw)
