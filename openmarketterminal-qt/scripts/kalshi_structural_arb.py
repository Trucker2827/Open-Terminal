#!/usr/bin/env python3
"""Read-only Kalshi structural-arbitrage recorder and replay auditor."""
from __future__ import annotations

import argparse
import json
import os
from decimal import Decimal
from pathlib import Path

from kalshi_microstructure.auth import DEFAULT_KEYS_PATH, load_credentials
from kalshi_microstructure.kalshi import KalshiRestClient
from kalshi_microstructure.structural_arb import (
    collect_snapshot,
    discover_candidates,
    load_certificate,
    ReadOnlyKalshiClient,
    record_snapshots,
    replay_evidence,
)


def _common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("certificate", help="Reviewed payoff-certificate JSON file.")
    parser.add_argument("--quantity", default="1", help="Equal contracts acquired per leg.")
    parser.add_argument("--execution-buffer", default="0.01")
    parser.add_argument("--min-net-edge", default="0.01")


def _values(args: argparse.Namespace) -> tuple[Decimal, Decimal, Decimal]:
    return Decimal(args.quantity), Decimal(args.execution_buffer), Decimal(args.min_net_edge)


def _append(path: Path, row: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
        handle.flush()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="kalshi-structural-arb",
        description="Certificate-driven Kalshi structural-arbitrage measurement. No order API.",
    )
    parser.add_argument("--env", choices=("prod", "demo"), default=os.getenv("KALSHI_ENV", "prod"))
    parser.add_argument("--keys", default=os.getenv("KALSHI_KEYS_PATH", str(DEFAULT_KEYS_PATH)))
    commands = parser.add_subparsers(dest="command", required=True)

    scan = commands.add_parser("scan", help="Capture and evaluate one batch snapshot.")
    _common(scan)
    scan.add_argument("--out", help="Optionally append the evidence row to JSONL.")

    record = commands.add_parser("record", help="Record repeated batch snapshots to JSONL.")
    _common(record)
    record.add_argument("--seconds", type=float, default=60.0)
    record.add_argument("--poll-seconds", type=float, default=1.0)
    record.add_argument("--out", required=True)

    replay = commands.add_parser("replay", help="Recompute and audit recorded evidence.")
    replay.add_argument("path")

    discover = commands.add_parser(
        "discover",
        help="Record open event candidates for manual rules review; certifies nothing.",
    )
    discover.add_argument("--out", required=True)
    discover.add_argument("--max-pages", type=int, default=0, help="0 means all pages.")
    discover.add_argument(
        "--page-size", type=int, default=20,
        help="Events per response; nested rule payloads are large (default: 20).",
    )

    args = parser.parse_args(argv)
    if args.command == "replay":
        report = replay_evidence(Path(args.path))
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if report["valid"] else 1

    if args.command == "discover":
        # Kalshi documents event/market discovery as public market data. Keep
        # this command independent of account credentials as well as orders.
        client = ReadOnlyKalshiClient(KalshiRestClient(env=args.env))
        try:
            report = discover_candidates(
                client, out=Path(args.out), max_pages=args.max_pages, page_size=args.page_size
            )
        except Exception as exc:  # noqa: BLE001 - CLI boundary, no false success.
            print(json.dumps({
                "status": "unavailable",
                "error": f"{type(exc).__name__}: {exc}",
                "candidates": 0,
                "certified": 0,
            }, indent=2, sort_keys=True))
            return 2
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    # Only read methods are exposed to the scanner. There is no facade method
    # for create_order/cancel_order and no bot/execution module import.
    client = ReadOnlyKalshiClient(
        KalshiRestClient(env=args.env, credentials=load_credentials(args.keys))
    )
    certificate = load_certificate(Path(args.certificate))
    quantity, execution_buffer, min_edge = _values(args)
    if args.command == "scan":
        row = collect_snapshot(
            client,
            certificate,
            quantity=quantity,
            execution_buffer_per_contract=execution_buffer,
            min_net_edge_per_bundle=min_edge,
        )
        if args.out:
            _append(Path(args.out), row)
        print(json.dumps(row["evaluation"], indent=2, sort_keys=True))
        return 0 if row["evaluation"]["state"] != "unavailable" else 2  # type: ignore[index]

    scans = record_snapshots(
        client,
        certificate,
        out=Path(args.out),
        seconds=args.seconds,
        poll_seconds=args.poll_seconds,
        quantity=quantity,
        execution_buffer_per_contract=execution_buffer,
        min_net_edge_per_bundle=min_edge,
    )
    print(f"recorded {scans} structural-arbitrage scans to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
