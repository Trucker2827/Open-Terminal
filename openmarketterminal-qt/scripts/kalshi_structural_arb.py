#!/usr/bin/env python3
"""Read-only Kalshi structural-arbitrage recorder and replay auditor."""
from __future__ import annotations

import argparse
import json
import os
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

from openterminal_paths import evidence_file

from kalshi_microstructure.auth import DEFAULT_KEYS_PATH, load_credentials
from kalshi_microstructure.kalshi import KalshiRestClient
from kalshi_microstructure.structural_arb import (
    BtcCorridorSeriesPolicy,
    collect_corridor_snapshot,
    collect_snapshot,
    derive_corridor_certificate,
    discover_candidates,
    load_certificate,
    load_corridor_certificate,
    load_corridor_series_policy,
    ReadOnlyKalshiClient,
    record_corridor_snapshots,
    record_snapshots,
    reviewed_kxbtcd_series_policy_payload,
    replay_evidence,
    rotate_corridor_snapshots,
    validate_corridor_series_metadata,
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

    corridor_scan = commands.add_parser(
        "corridor-scan",
        help="Evaluate every pair in a reviewed BTC threshold-corridor family.",
    )
    _common(corridor_scan)
    corridor_scan.add_argument("--out", help="Optionally append the evidence row to JSONL.")

    corridor_record = commands.add_parser(
        "corridor-record",
        help="Record repeated reviewed BTC threshold-corridor snapshots.",
    )
    _common(corridor_record)
    corridor_record.add_argument("--seconds", type=float, default=60.0)
    corridor_record.add_argument("--poll-seconds", type=float, default=1.0)
    corridor_record.add_argument(
        "--out",
        default=evidence_file("kalshi-btc-threshold-corridor.jsonl"),
        help="Evidence JSONL (default: Bot Cockpit evidence directory).",
    )

    policy_create = commands.add_parser(
        "corridor-policy-create",
        help="Validate a reviewed hourly KXBTCD reference and write one immutable series policy.",
    )
    policy_create.add_argument("reference_event")
    policy_create.add_argument("--out", required=True)

    corridor_rotate = commands.add_parser(
        "corridor-rotate",
        help="Derive and overlap hourly event certificates under a reviewed series policy.",
    )
    corridor_rotate.add_argument("policy")
    corridor_rotate.add_argument("--quantity", default="1")
    corridor_rotate.add_argument("--execution-buffer", default="0.01")
    corridor_rotate.add_argument("--min-net-edge", default="0.01")
    corridor_rotate.add_argument("--seconds", type=float, default=3600.0)
    corridor_rotate.add_argument("--poll-seconds", type=float, default=10.0)
    corridor_rotate.add_argument("--max-evidence-bytes", type=int, default=64 * 1024 * 1024)
    corridor_rotate.add_argument(
        "--out", default=evidence_file("kalshi-btc-threshold-corridor.jsonl")
    )
    corridor_rotate.add_argument(
        "--status-out", default=evidence_file("kalshi-btc-corridor-rotation-status.json")
    )

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
    if args.command == "corridor-policy-create":
        output = Path(args.out)
        if output.exists():
            print(json.dumps({"status": "refused", "error": "policy already exists"}, indent=2))
            return 3
        client = ReadOnlyKalshiClient(KalshiRestClient(env=args.env))
        reviewed_at = datetime.now(timezone.utc).isoformat()
        try:
            payload = reviewed_kxbtcd_series_policy_payload(
                reviewed_at=reviewed_at, reviewed_reference_event=args.reference_event
            )
            policy = BtcCorridorSeriesPolicy.from_payload(payload)
            validate_corridor_series_metadata(
                policy, client.get_series(policy.series_ticker)
            )
            event = client.get_event(args.reference_event, with_nested_markets=True)
            certificate = derive_corridor_certificate(policy, event)
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("x", encoding="utf-8") as handle:
                handle.write(json.dumps(policy.payload(), indent=2, sort_keys=True) + "\n")
        except Exception as exc:  # noqa: BLE001 - never write a partial authority.
            print(json.dumps({"status": "refused", "error": f"{type(exc).__name__}: {exc}"}, indent=2))
            return 2
        print(json.dumps({
            "status": "created", "path": str(output),
            "series_policy_sha256": policy.digest,
            "reference_certificate_sha256": certificate.digest,
            "reference_markets": len(certificate.members),
        }, indent=2, sort_keys=True))
        return 0
    if args.command == "corridor-rotate":
        client = ReadOnlyKalshiClient(KalshiRestClient(env=args.env))
        try:
            policy = load_corridor_series_policy(Path(args.policy))
            scans = rotate_corridor_snapshots(
                client, policy, out=Path(args.out), status_out=Path(args.status_out),
                seconds=args.seconds, poll_seconds=args.poll_seconds,
                quantity=Decimal(args.quantity),
                execution_buffer_per_contract=Decimal(args.execution_buffer),
                min_net_edge_per_bundle=Decimal(args.min_net_edge),
                max_evidence_bytes=args.max_evidence_bytes,
            )
        except Exception as exc:  # noqa: BLE001 - no false-success service exit.
            print(json.dumps({"status": "unavailable", "error": f"{type(exc).__name__}: {exc}"}, indent=2))
            return 2
        print(f"recorded {scans} policy-derived BTC corridor scans to {args.out}")
        return 0
    # Only read methods are exposed to the scanner. There is no facade method
    # for create_order/cancel_order and no bot/execution module import.
    corridor_command = args.command in {"corridor-scan", "corridor-record"}
    client = ReadOnlyKalshiClient(
        KalshiRestClient(
            env=args.env,
            credentials=None if corridor_command else load_credentials(args.keys),
        )
    )
    quantity, execution_buffer, min_edge = _values(args)
    if args.command in {"corridor-scan", "corridor-record"}:
        certificate = load_corridor_certificate(Path(args.certificate))
        if args.command == "corridor-scan":
            row = collect_corridor_snapshot(
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
        scans = record_corridor_snapshots(
            client,
            certificate,
            out=Path(args.out),
            seconds=args.seconds,
            poll_seconds=args.poll_seconds,
            quantity=quantity,
            execution_buffer_per_contract=execution_buffer,
            min_net_edge_per_bundle=min_edge,
        )
        print(f"recorded {scans} BTC threshold-corridor scans to {args.out}")
        return 0

    certificate = load_certificate(Path(args.certificate))
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
