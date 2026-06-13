"""Live AIS vessel feed via AISStream.io (free, requires a free API key).

A long-running streaming subprocess (same shape as exchange/ws_stream.py): it
opens a WebSocket to AISStream.io, merges PositionReport + ShipStaticData into
per-vessel records, and prints one JSON object per line on stdout. The C++
AisStreamFeed reads those lines and publishes them to the maritime DataHub
topics the screen + dashboard widget already consume.

Key:   env OPENMARKETTERMINAL_AISSTREAM_KEY  (never passed on argv / committed)
Box:   env OPENMARKETTERMINAL_AIS_BBOX = "minLat,minLon,maxLat,maxLon" (default global)

stdout (one JSON object per line):
  {"mmsi": int, "imo": "<str>", "name": "...", "lat": .., "lon": ..,
   "speed": .. (knots), "angle": .. (course °), "to_port": "..."}
A line {"ready": true} is printed once the subscription is live.
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import time

import websockets

WS_URL = "wss://stream.aisstream.io/v0/stream"
EMIT_THROTTLE_S = 30.0   # at most one line per vessel per this window
RECONNECT_S = 5.0


def _bbox():
    raw = os.environ.get("OPENMARKETTERMINAL_AIS_BBOX", "").strip()
    if raw:
        try:
            a, b, c, d = (float(x) for x in raw.split(","))
            return [[[a, b], [c, d]]]
        except Exception:
            pass
    return [[[-90.0, -180.0], [90.0, 180.0]]]   # whole world


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


async def run(key: str) -> None:
    bbox = _bbox()
    # MMSI -> static info learned from ShipStaticData (imo, destination).
    static: dict[int, dict] = {}
    last_emit: dict[int, float] = {}

    while True:
        try:
            async with websockets.connect(WS_URL, open_timeout=20, ping_interval=20) as ws:
                await ws.send(json.dumps({
                    "APIKey": key,
                    "BoundingBoxes": bbox,
                    "FilterMessageTypes": ["PositionReport", "ShipStaticData"],
                }))
                _emit({"ready": True})
                async for raw in ws:
                    try:
                        m = json.loads(raw)
                    except Exception:
                        continue
                    mt = m.get("MessageType")
                    md = m.get("MetaData", {}) or {}
                    mmsi = md.get("MMSI")
                    if not mmsi:
                        continue

                    if mt == "ShipStaticData":
                        sd = m.get("Message", {}).get("ShipStaticData", {}) or {}
                        imo = sd.get("ImoNumber") or 0
                        static[mmsi] = {
                            "imo": str(imo) if imo else "",
                            "to_port": (sd.get("Destination") or "").strip(),
                        }
                        continue

                    if mt != "PositionReport":
                        continue
                    now = time.monotonic()
                    if now - last_emit.get(mmsi, 0.0) < EMIT_THROTTLE_S:
                        continue
                    pr = m.get("Message", {}).get("PositionReport", {}) or {}
                    name = (md.get("ShipName") or "").strip()
                    if not name:
                        continue   # skip un-named targets (buoys, etc.) to keep the list useful
                    last_emit[mmsi] = now
                    info = static.get(mmsi, {})
                    _emit({
                        "mmsi": mmsi,
                        "imo": info.get("imo", ""),
                        "name": name,
                        "lat": pr.get("Latitude"),
                        "lon": pr.get("Longitude"),
                        "speed": pr.get("Sog"),     # knots
                        "angle": pr.get("Cog"),     # course over ground
                        "to_port": info.get("to_port", ""),
                    })
        except Exception as e:
            _emit({"error": f"{type(e).__name__}: {e}"})
            await asyncio.sleep(RECONNECT_S)


def main() -> int:
    key = os.environ.get("OPENMARKETTERMINAL_AISSTREAM_KEY", "").strip()
    if not key:
        _emit({"error": "no AISStream API key configured (connectors.aisstream_key)"})
        return 1
    try:
        asyncio.run(run(key))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
