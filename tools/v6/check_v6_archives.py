#!/usr/bin/env python3
"""Reject legacy UCN symbols and archive names from a v6 build tree."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


SYMBOL = re.compile(r"\b(ucn_[A-Za-z0-9_]+)$")

SIMPLIFIED_PUBLIC = {
    "ucn_storage_required", "ucn_init", "ucn_start", "ucn_step",
    "ucn_stop", "ucn_deinit", "ucn_endpoint_add", "ucn_endpoint_remove",
    "ucn_static_path_add", "ucn_static_path_remove", "ucn_link_get",
    "ucn_publish", "ucn_send_query", "ucn_send_cancel", "ucn_send_forget",
    "ucn_get_stats", "ucn_driver_rx_publish", "ucn_driver_tx_complete",
    "ucn_driver_link_event", "ucn_callback_link_get",
    "ucn_callback_send_query", "ucn_callback_get_stats",
}


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
        is_v6_module = (archive.name.startswith("libucn_v6_") or
                        archive.name.startswith("ucn_v6_"))
        is_simplified_common = archive.name in (
            "libucn_common.a", "ucn_common.lib",
            "libucn_coordinator.a", "ucn_coordinator.lib",
            "libucn_wire.a", "ucn_wire.lib",
            "libucn_adapter.a", "ucn_adapter.lib")
        is_simplified_kernel = archive.name in (
            "libucn_kernel.a", "ucn_kernel.lib",
            "libucn_simplified.a", "ucn_simplified.lib")
        is_persistence_adapter = archive.name in (
            "libucn_persistence_coordinator.a",
            "ucn_persistence_coordinator.lib")
        is_simplified_persistence = archive.name in (
            "libucn_persistence.a", "ucn_persistence.lib",
            "libucn_persistence_coordinator.a",
            "ucn_persistence_coordinator.lib")
        if not (is_v6_module or is_simplified_common or is_simplified_kernel
                or is_simplified_persistence):
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
            if match:
                symbol = match.group(1)
                if is_v6_module:
                    allowed = symbol.startswith("ucn_v6_")
                elif is_simplified_persistence:
                    allowed = (symbol.startswith("ucn_persistence_") or
                               symbol.startswith("ucn_persist_") or
                               symbol.startswith("ucn_i_persist_") or
                               symbol.startswith("ucn_i_persistence_"))
                elif is_simplified_kernel:
                    # The installed aggregate owns both the public facade and
                    # its private implementation symbols.  The test-only
                    # kernel owns only the facade and unresolved ucn_i_ calls.
                    allowed = (symbol in SIMPLIFIED_PUBLIC or
                               (archive.name in (
                                   "libucn_simplified.a", "ucn_simplified.lib") and
                                symbol.startswith("ucn_i_")))
                else:
                    allowed = symbol.startswith("ucn_i_")
                if not allowed:
                    errors.append(
                        f"legacy symbol in {archive.name}: {symbol}")
        if tool_name.startswith("dumpbin"):
            undefined_command = [
                args.symbol_tool, "/nologo", "/symbols", str(archive)
            ]
        else:
            undefined_command = [args.symbol_tool, "-u", str(archive)]
        undefined = subprocess.run(
            undefined_command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if undefined.returncode != 0:
            errors.append(
                f"undefined-symbol scan failed for {archive.name}: "
                f"{undefined.stderr.strip()}"
            )
            continue
        for line in undefined.stdout.splitlines():
            if tool_name.startswith("dumpbin") and " UNDEF " not in line:
                continue
            match = SYMBOL.search(line.strip())
            if match:
                symbol = match.group(1)
                if is_v6_module:
                    allowed = symbol.startswith("ucn_v6_")
                elif is_simplified_persistence:
                    allowed = ((is_persistence_adapter and
                                symbol.startswith("ucn_i_")) or
                               symbol.startswith("ucn_i_persist_") or
                               symbol.startswith("ucn_i_persistence_") or
                               symbol.startswith("ucn_persistence_") or
                               symbol.startswith("ucn_persist_"))
                elif is_simplified_kernel:
                    allowed = symbol.startswith("ucn_i_")
                else:
                    allowed = symbol.startswith("ucn_i_")
                if not allowed:
                    errors.append(
                        f"foreign UCN dependency in {archive.name}: {symbol}"
                    )
    if errors:
        for error in errors:
            print(f"V6_ARCHIVE_ERROR {error}")
        return 1
    print(f"V6_ARCHIVE_OK archives={len(archives)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
