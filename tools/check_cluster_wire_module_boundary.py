#!/usr/bin/env python3
"""M14 gate: v3 compatibility and strict-v4 codec remain separate modules."""

from __future__ import annotations

import pathlib
import re
import sys


def fail(message: str) -> None:
    print(f"M14_CLUSTER_WIRE_MODULE_GATE: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: check_cluster_wire_module_boundary.py <repo-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    header = (root / "include/ucn/ucn_cluster_wire_v4.h").read_text(
        encoding="utf-8"
    )
    block = re.search(
        r"add_library\(ucn_cluster STATIC EXCLUDE_FROM_ALL(.*?)\n\)",
        cmake,
        flags=re.S,
    )
    if block is None:
        fail("cannot locate ucn_cluster source list")
    if "ucn_cluster_codec_v3.c" in block.group(1) or "ucn_cluster_codec_v4.c" in block.group(1):
        fail("wire codec leaked back into Current Cluster archive source list")
    required = (
        "UCN_CLUSTER_ENABLE_V3_COMPAT",
        "add_library(ucn_cluster_wire_v3_compat",
        "add_library(ucn_cluster_wire_v4",
        "add_library(ucn_cluster_wire_dual_stack",
        "src/extended/cluster/ucn_cluster_wire_dispatch.c",
    )
    for token in required:
        if token not in cmake:
            fail(f"missing module contract: {token}")
    codec_v4 = (
        root / "src/extended/cluster/ucn_cluster_codec_v4.c"
    ).read_text(encoding="utf-8")
    if "ucn_cluster_message_decode(" in codec_v4:
        fail("strict-v4 codec still references the legacy v3 decoder")
    if "#define UCN_CLUSTER_RECOMMENDED_WIRE_FORMAT" not in header:
        fail("strict-v4 recommendation is missing")
    print("M14_CLUSTER_WIRE_MODULE_GATE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
