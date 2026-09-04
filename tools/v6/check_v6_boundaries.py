#!/usr/bin/env python3
"""Fail-closed source boundary checks for the active UCN v6 implementation."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    errors: list[str] = []
    runtime_files = sorted((root / "src" / "v6").rglob("*.c"))
    public_files = sorted((root / "include" / "ucn" / "v6").rglob("*.h"))
    if not runtime_files or not public_files:
        errors.append("v6 source/header set is empty")

    heap_pattern = re.compile(r"\b(malloc|calloc|realloc|free|alloca)\s*\(")
    old_include = re.compile(r"#\s*include\s*[<\"]ucn/(?!v6/)")
    for path in runtime_files + public_files:
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(root).as_posix()
        if heap_pattern.search(text):
            errors.append(f"runtime heap API in {relative}")
        if old_include.search(text):
            errors.append(f"legacy public include in {relative}")
        if "TODO" in text or "FIXME" in text:
            errors.append(f"unfinished marker in {relative}")

    cluster_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in runtime_files + public_files
        if "cluster" in path.name
    )
    realtime_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in runtime_files + public_files
        if "realtime" in path.name
    )
    if re.search(r"ucn_v6_realtime", cluster_text):
        errors.append("Cluster depends on Realtime")
    if re.search(r"ucn_v6_cluster", realtime_text):
        errors.append("Realtime depends on Cluster")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    required_targets = (
        "ucn_v6_config", "ucn_v6_identity", "ucn_v6_wire",
        "ucn_v6_message", "ucn_v6_owner", "ucn_v6_security",
        "ucn_v6_capability", "ucn_v6_route", "ucn_v6_qos",
        "ucn_v6_transfer", "ucn_v6_realtime", "ucn_v6_cluster",
        "ucn_v6_adapter",
    )
    for target in required_targets:
        if f"add_library({target} " not in cmake:
            errors.append(f"missing CMake target {target}")
    if not re.search(
        r"option\s*\(\s*UCN_BUILD_V6_EXPERIMENTAL\b[\s\S]*?\bOFF\s*\)",
        cmake,
    ):
        errors.append("v6 default-OFF build fence is missing")

    required_bearer_files = (
        "src/v6/adapter/ucn_v6_uart.c",
        "src/v6/adapter/ucn_v6_wifi.c",
        "src/v6/adapter/ucn_v6_can.c",
        "src/v6/adapter/ucn_v6_usb.c",
        "src/v6/ports/ucn_v6_freertos.c",
    )
    for relative in required_bearer_files:
        if not (root / relative).is_file():
            errors.append(f"missing isolated adapter/port file {relative}")

    if errors:
        for error in errors:
            print(f"V6_BOUNDARY_ERROR: {error}")
        return 1
    print(
        "V6_BOUNDARY_OK "
        f"runtime_files={len(runtime_files)} public_headers={len(public_files)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
