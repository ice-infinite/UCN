#!/usr/bin/env python3
"""Reject legacy UCN symbols and archive names from a v6 build tree."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


SYMBOL = re.compile(r"\b(ucn_[A-Za-z0-9_]+)$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--symbol-tool", required=True)
    args = parser.parse_args()
    build_dir = Path(args.build_dir).resolve()
    # Single-config generators place archives at the build root, while Visual
    # Studio places them below the selected configuration directory.  Inspect
    # the complete build tree so this release gate has identical semantics on
    # GCC/Clang and MSVC.
    archives = sorted(build_dir.rglob("libucn*.a"))
    archives.extend(sorted(build_dir.rglob("ucn*.lib")))
    errors: list[str] = []
    if not archives:
        errors.append("no UCN static archives found")
    for archive in archives:
        if not (archive.name.startswith("libucn_v6_") or
                archive.name.startswith("ucn_v6_")):
            errors.append(f"legacy archive name: {archive.name}")
            continue
        tool_name = Path(args.symbol_tool).name.lower()
        if tool_name.startswith("dumpbin"):
            command = [args.symbol_tool, "/nologo", "/linkermember:1", str(archive)]
        else:
            command = [args.symbol_tool, "-g", "--defined-only", str(archive)]
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if result.returncode != 0:
            errors.append(
                f"symbol tool failed for {archive.name}: {result.stderr.strip()}"
            )
            continue
        for line in result.stdout.splitlines():
            match = SYMBOL.search(line.strip())
            if match and not match.group(1).startswith("ucn_v6_"):
                errors.append(f"legacy symbol in {archive.name}: {match.group(1)}")
    if errors:
        for error in errors:
            print(f"V6_ARCHIVE_ERROR {error}")
        return 1
    print(f"V6_ARCHIVE_OK archives={len(archives)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
