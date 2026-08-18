#!/usr/bin/env python3
"""Build and install a fail-closed, credential-free public market-data seed.

The source application database is never copied.  Export creates a brand-new
SQLite database from an exact table/column allowlist, scans every exported text
value for secret material and local identities, and emits a signed-by-checksum
manifest beside the database in a ZIP bundle.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path


POLICY_VERSION = 1
DATABASE_NAME = "public-seed.sqlite"
MANIFEST_NAME = "manifest.json"
MAX_DOWNLOAD_BYTES = 8 * 1024 * 1024 * 1024

# This is the security boundary. New source tables and columns are rejected
# until they are deliberately reviewed and added here.
ALLOWLIST: dict[str, tuple[str, ...]] = {
    "edge_prediction_raw_ticks": (
        "id", "symbol", "source", "price", "exchange_ts", "received_ts",
    ),
    "edge_prediction_market_snapshots": (
        "id", "venue", "symbol", "horizon", "market_id", "question",
        "yes_price", "no_price", "spread_cost", "liquidity_score",
        "seconds_left", "observed_at",
    ),
    "market_data": (
        "symbol", "exchange", "interval", "timestamp_ms", "open", "high",
        "low", "close", "volume", "oi",
    ),
}

FORBIDDEN_NAME = re.compile(
    r"(^|_)(api_?key|secret|token|cookie|authorization|request_?headers?|"
    r"account(_?id)?|portfolio(_?id)?|orders?|order_?id|client_?order(_?id)?|"
    r"fills?|fill_?id|username|user_?name|home_?path|file_?path)($|_)", re.I,
)
SECRET_VALUE_PATTERNS = (
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(r"\b(?:authorization|x-api-key|api[_-]?key|access[_-]?token|"
               r"refresh[_-]?token|client[_-]?secret|cookie|set-cookie)\s*[:=]", re.I),
    re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{12,}", re.I),
    re.compile(r"\b(?:sk|pk)_(?:live|test)_[A-Za-z0-9]{12,}\b"),
)
PATH_PATTERNS = (
    re.compile(r"(?:^|[\s\"'])(?:/Users|/home)/[^/\s\"']+[/\\]"),
    re.compile(r"(?:^|[\s\"'])[A-Za-z]:\\Users\\[^\\\s\"']+\\", re.I),
    re.compile(r"(?:^|[\s\"'])~[/\\]"),
)


class SeedSecurityError(RuntimeError):
    pass


def _quote(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _reject_name(name: str, context: str) -> None:
    if FORBIDDEN_NAME.search(name):
        raise SeedSecurityError(f"forbidden {context}: {name}")


def _scan_json_keys(value: object, context: str) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            _reject_name(str(key), f"JSON key in {context}")
            _scan_json_keys(child, context)
    elif isinstance(value, list):
        for child in value:
            _scan_json_keys(child, context)


def scan_text(value: str, context: str) -> None:
    for pattern in SECRET_VALUE_PATTERNS:
        if pattern.search(value):
            raise SeedSecurityError(f"secret-like value rejected at {context}")
    for pattern in PATH_PATTERNS:
        if pattern.search(value):
            raise SeedSecurityError(f"local filesystem path rejected at {context}")
    local_names = {getpass.getuser(), Path.home().name,
                   os.environ.get("USER", ""), os.environ.get("USERNAME", ""),
                   os.environ.get("LOGNAME", "")}
    for name in {item.strip() for item in local_names if len(item.strip()) >= 3}:
        if re.search(rf"(?<![A-Za-z0-9]){re.escape(name)}(?![A-Za-z0-9])", value, re.I):
            raise SeedSecurityError(f"local username rejected at {context}")
    stripped = value.strip()
    if stripped.startswith(("{", "[")):
        try:
            _scan_json_keys(json.loads(stripped), context)
        except json.JSONDecodeError:
            pass


def validate_selection(tables: list[str] | None) -> list[str]:
    selected = list(ALLOWLIST) if not tables else tables
    if not selected:
        raise SeedSecurityError("at least one allowlisted table is required")
    for table in selected:
        _reject_name(table, "table")
        if table not in ALLOWLIST:
            raise SeedSecurityError(f"table is not explicitly allowlisted: {table}")
        for column in ALLOWLIST[table]:
            _reject_name(column, f"column in {table}")
    return selected


def _source_schema(connection: sqlite3.Connection, table: str) -> dict[str, str]:
    rows = connection.execute(f"PRAGMA table_info({_quote(table)})").fetchall()
    if not rows:
        raise SeedSecurityError(f"allowlisted source table is missing: {table}")
    return {str(row[1]): str(row[2] or "").strip() or "TEXT" for row in rows}


def export_database(source: Path, destination: Path,
                    tables: list[str] | None = None) -> dict[str, object]:
    selected = validate_selection(tables)
    if not source.is_file():
        raise SeedSecurityError(f"source database does not exist: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise SeedSecurityError(f"refusing to overwrite destination: {destination}")

    source_uri = f"file:{urllib.parse.quote(str(source.resolve()))}?mode=ro"
    src = sqlite3.connect(source_uri, uri=True)
    dst = sqlite3.connect(destination)
    summary: dict[str, object] = {}
    try:
        dst.execute("PRAGMA journal_mode=OFF")
        dst.execute("PRAGMA synchronous=OFF")
        dst.execute("PRAGMA page_size=4096")
        for table in selected:
            schema = _source_schema(src, table)
            columns = ALLOWLIST[table]
            missing = [column for column in columns if column not in schema]
            if missing:
                raise SeedSecurityError(
                    f"allowlisted columns missing from {table}: {', '.join(missing)}")
            unexpected = [column for column in schema if column not in columns]
            if unexpected:
                raise SeedSecurityError(
                    f"source columns are not explicitly allowlisted for {table}: "
                    f"{', '.join(unexpected)}")
            definitions = ", ".join(
                f"{_quote(column)} {schema[column]}" for column in columns)
            dst.execute(f"CREATE TABLE {_quote(table)} ({definitions})")
            select_sql = (
                f"SELECT {', '.join(_quote(c) for c in columns)} "
                f"FROM {_quote(table)} ORDER BY rowid")
            insert_sql = (
                f"INSERT INTO {_quote(table)} VALUES "
                f"({', '.join('?' for _ in columns)})")
            count = 0
            for row in src.execute(select_sql):
                for index, value in enumerate(row):
                    if isinstance(value, str):
                        scan_text(value, f"{table}.{columns[index]} row {count + 1}")
                dst.execute(insert_sql, row)
                count += 1
            summary[table] = {"columns": list(columns), "rows": count}
        dst.commit()
        dst.execute("VACUUM")
        dst.commit()
    except Exception:
        dst.close()
        src.close()
        destination.unlink(missing_ok=True)
        raise
    finally:
        try:
            dst.close()
        except Exception:
            pass
        try:
            src.close()
        except Exception:
            pass
    return summary


def _canonical_json(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def create_bundle(source: Path, bundle: Path,
                  tables: list[str] | None = None) -> dict[str, object]:
    if bundle.exists():
        raise SeedSecurityError(f"refusing to overwrite bundle: {bundle}")
    bundle.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="openterminal-public-seed-") as temporary:
        root = Path(temporary)
        database = root / DATABASE_NAME
        table_summary = export_database(source, database, tables)
        manifest = {
            "format": "openterminal-public-seed",
            "policy_version": POLICY_VERSION,
            "database_file": DATABASE_NAME,
            "database_sha256": sha256_file(database),
            "database_bytes": database.stat().st_size,
            "tables": table_summary,
        }
        (root / MANIFEST_NAME).write_bytes(_canonical_json(manifest))
        temporary_bundle = root / "bundle.zip"
        with zipfile.ZipFile(temporary_bundle, "w", zipfile.ZIP_DEFLATED) as archive:
            for name in (DATABASE_NAME, MANIFEST_NAME):
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o600 << 16
                archive.writestr(info, (root / name).read_bytes())
        os.replace(temporary_bundle, bundle)
    return {**manifest, "bundle_sha256": sha256_file(bundle), "bundle": str(bundle)}


def _read_bundle(bundle: Path) -> tuple[dict[str, object], bytes]:
    if not bundle.is_file():
        raise SeedSecurityError(f"bundle does not exist: {bundle}")
    with zipfile.ZipFile(bundle, "r") as archive:
        names = archive.namelist()
        if sorted(names) != sorted((DATABASE_NAME, MANIFEST_NAME)):
            raise SeedSecurityError(f"unexpected bundle members: {names}")
        for info in archive.infolist():
            if info.is_dir() or Path(info.filename).name != info.filename:
                raise SeedSecurityError(f"unsafe bundle member: {info.filename}")
        try:
            manifest = json.loads(archive.read(MANIFEST_NAME))
            database = archive.read(DATABASE_NAME)
        except (KeyError, json.JSONDecodeError) as exc:
            raise SeedSecurityError(f"invalid seed bundle: {exc}") from exc
    if manifest.get("format") != "openterminal-public-seed":
        raise SeedSecurityError("unrecognized seed format")
    if manifest.get("policy_version") != POLICY_VERSION:
        raise SeedSecurityError("unsupported seed policy version")
    if manifest.get("database_file") != DATABASE_NAME:
        raise SeedSecurityError("manifest database filename mismatch")
    if manifest.get("database_bytes") != len(database):
        raise SeedSecurityError("manifest database size mismatch")
    actual = hashlib.sha256(database).hexdigest()
    if manifest.get("database_sha256") != actual:
        raise SeedSecurityError("manifest database checksum mismatch")
    tables = manifest.get("tables")
    if not isinstance(tables, dict):
        raise SeedSecurityError("manifest tables are missing")
    validate_selection(list(tables))
    for table, expected_columns in ALLOWLIST.items():
        if table in tables and tables[table].get("columns") != list(expected_columns):
            raise SeedSecurityError(f"manifest columns are not allowlisted for {table}")
    return manifest, database


def verify_bundle(bundle: Path) -> dict[str, object]:
    manifest, database_bytes = _read_bundle(bundle)
    with tempfile.TemporaryDirectory(prefix="openterminal-seed-verify-") as temporary:
        database = Path(temporary) / DATABASE_NAME
        database.write_bytes(database_bytes)
        connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
        try:
            actual_tables = {
                row[0] for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'")
            }
            expected_tables = set(manifest["tables"])
            if actual_tables != expected_tables:
                raise SeedSecurityError(
                    f"database tables differ from manifest: {sorted(actual_tables)}")
            for table, details in manifest["tables"].items():
                schema = _source_schema(connection, table)
                if list(schema) != details["columns"]:
                    raise SeedSecurityError(f"database columns differ for {table}")
                count = connection.execute(
                    f"SELECT COUNT(*) FROM {_quote(table)}").fetchone()[0]
                if count != details["rows"]:
                    raise SeedSecurityError(f"database row count differs for {table}")
                for row in connection.execute(
                        f"SELECT * FROM {_quote(table)} ORDER BY rowid"):
                    for index, value in enumerate(row):
                        if isinstance(value, str):
                            scan_text(value, f"{table}.{details['columns'][index]}")
        finally:
            connection.close()
    return {**manifest, "bundle_sha256": sha256_file(bundle), "verified": True}


def download_bundle(url: str, destination: Path, expected_sha256: str,
                    max_bytes: int = MAX_DOWNLOAD_BYTES) -> dict[str, object]:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https":
        raise SeedSecurityError("public seed downloads require HTTPS")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha256):
        raise SeedSecurityError("a valid expected SHA-256 is required")
    if destination.exists():
        raise SeedSecurityError(f"refusing to overwrite download: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".part")
    temporary.unlink(missing_ok=True)
    digest = hashlib.sha256()
    total = 0
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "OpenTerminal-PublicSeed/1"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("xb") as output:
            for chunk in iter(lambda: response.read(1024 * 1024), b""):
                total += len(chunk)
                if total > max_bytes:
                    raise SeedSecurityError("public seed download exceeds size limit")
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest().lower() != expected_sha256.lower():
            raise SeedSecurityError("download checksum mismatch")
        verify_bundle(temporary)
        os.replace(temporary, destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return {"download": str(destination), "bytes": total,
            "bundle_sha256": digest.hexdigest(), "verified": True}


def install_bundle(bundle: Path, data_directory: Path) -> dict[str, object]:
    manifest, database = _read_bundle(bundle)
    data_directory.mkdir(parents=True, exist_ok=True)
    target = data_directory / DATABASE_NAME
    target_manifest = data_directory / "public-seed-manifest.json"
    if target.exists():
        existing = sha256_file(target)
        if existing == manifest["database_sha256"]:
            return {"installed": False, "reason": "already_installed", "database": str(target)}
        raise SeedSecurityError(f"refusing to overwrite existing public seed: {target}")
    temporary = target.with_name(target.name + ".part")
    temporary.write_bytes(database)
    if sha256_file(temporary) != manifest["database_sha256"]:
        temporary.unlink(missing_ok=True)
        raise SeedSecurityError("staged database checksum mismatch")
    os.replace(temporary, target)
    target_manifest.write_bytes(_canonical_json(manifest))
    return {"installed": True, "database": str(target),
            "manifest": str(target_manifest), "database_sha256": manifest["database_sha256"]}


def stage_bundle(bundle: Path, output_directory: Path) -> dict[str, object]:
    """Create the exact loose files consumed by native first-launch import."""
    manifest, database = _read_bundle(bundle)
    output_directory.mkdir(parents=True, exist_ok=True)
    target_db = output_directory / DATABASE_NAME
    target_manifest = output_directory / MANIFEST_NAME
    if target_db.exists() or target_manifest.exists():
        raise SeedSecurityError(f"refusing to overwrite staged public seed: {output_directory}")
    db_part = output_directory / (DATABASE_NAME + ".part")
    manifest_part = output_directory / (MANIFEST_NAME + ".part")
    try:
        db_part.write_bytes(database)
        manifest_part.write_bytes(_canonical_json(manifest))
        if sha256_file(db_part) != manifest["database_sha256"]:
            raise SeedSecurityError("staged database checksum mismatch")
        os.replace(db_part, target_db)
        os.replace(manifest_part, target_manifest)
    except Exception:
        db_part.unlink(missing_ok=True)
        manifest_part.unlink(missing_ok=True)
        raise
    return {"staged": True, "directory": str(output_directory),
            "database_sha256": manifest["database_sha256"]}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("export", help="create a sanitized public seed bundle")
    export.add_argument("--source", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--table", action="append", dest="tables")
    verify = sub.add_parser("verify", help="verify a seed bundle and rescan its contents")
    verify.add_argument("--bundle", type=Path, required=True)
    download = sub.add_parser("download", help="download an HTTPS seed with a pinned checksum")
    download.add_argument("--url", required=True)
    download.add_argument("--sha256", required=True)
    download.add_argument("--output", type=Path, required=True)
    install = sub.add_parser("install", help="first-launch install into a separate data DB")
    install.add_argument("--bundle", type=Path, required=True)
    install.add_argument("--data-dir", type=Path, required=True)
    stage = sub.add_parser("stage", help="stage verified loose files for application packaging")
    stage.add_argument("--bundle", type=Path, required=True)
    stage.add_argument("--output-dir", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "export":
            result = create_bundle(args.source, args.output, args.tables)
        elif args.command == "verify":
            result = verify_bundle(args.bundle)
        elif args.command == "download":
            result = download_bundle(args.url, args.output, args.sha256)
        elif args.command == "install":
            result = install_bundle(args.bundle, args.data_dir)
        else:
            result = stage_bundle(args.bundle, args.output_dir)
        print(json.dumps({"ok": True, **result}, sort_keys=True))
        return 0
    except (SeedSecurityError, sqlite3.Error, OSError, zipfile.BadZipFile) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, sort_keys=True), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
