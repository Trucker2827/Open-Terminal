from __future__ import annotations

import json
from urllib.error import URLError
from urllib.request import Request, urlopen


def reference_spot(symbol: str = "BTC-USD", timeout: float = 5.0) -> float:
    errors: list[str] = []
    for provider in (_coinbase_spot, _binance_spot):
        try:
            return provider(symbol, timeout)
        except (KeyError, ValueError, URLError, TimeoutError) as exc:
            errors.append(f"{provider.__name__}: {exc}")
    raise RuntimeError("Unable to fetch reference spot price: " + "; ".join(errors))


def _coinbase_spot(symbol: str, timeout: float) -> float:
    product = symbol.upper().replace("USDT", "USD")
    if "-" not in product and product.endswith("USD"):
        product = f"{product[:-3]}-USD"
    request = Request(
        f"https://api.coinbase.com/v2/prices/{product}/spot",
        headers={"accept": "application/json", "user-agent": "kalshi-microstructure/0.1"},
    )
    with urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    return float(payload["data"]["amount"])


def _binance_spot(symbol: str, timeout: float) -> float:
    product = symbol.upper().replace("-", "")
    request = Request(
        f"https://api.binance.com/api/v3/ticker/price?symbol={product}",
        headers={"accept": "application/json", "user-agent": "kalshi-microstructure/0.1"},
    )
    with urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    return float(payload["price"])
