#!/usr/bin/env python3
"""Fail when a production v6 C function exceeds the fixed stack budget."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--limit-bytes", required=True, type=int)
    parser.add_argument(
        "--source-prefix",
        action="append",
        help="normalized source-path fragment whose records are in scope",
    )
    args = parser.parse_args()
    prefixes = [item.replace("\\", "/") for item in
                (args.source_prefix or ["/src/v6/"])]

    records: list[tuple[int, str]] = []
    invalid: list[str] = []
    for path in args.build_dir.rglob("*.su"):
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            fields = raw.split("\t")
            if len(fields) < 3:
                continue
            location = fields[0].replace("\\", "/")
            if not any(prefix in location for prefix in prefixes):
                continue
            try:
                stack_bytes = int(fields[1])
            except ValueError:
                invalid.append(raw)
                continue
            if fields[2].strip() not in {"static", "dynamic,bounded"}:
                invalid.append(raw)
            records.append((stack_bytes, raw))

    if not records:
        print(
            "stack gate: no production .su records found for "
            f"{prefixes}"
        )
        return 2
    offenders = [item for item in records if item[0] > args.limit_bytes]
    if invalid or offenders:
        for raw in invalid:
            print(f"unbounded/invalid stack record: {raw}")
        for stack_bytes, raw in sorted(offenders, reverse=True):
            print(f"stack {stack_bytes} > {args.limit_bytes}: {raw}")
        return 1
    maximum = max(records)
    print(
        f"stack gate: {len(records)} functions from {prefixes}, "
        f"max={maximum[0]} bytes, limit={args.limit_bytes} bytes"
    )
    print(maximum[1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
