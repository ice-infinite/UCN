#!/usr/bin/env python3
"""CLV2-14-10: keep public Cluster enums/constants and canonical docs aligned."""

from __future__ import annotations

import pathlib
import re
import sys


def fail(message: str) -> None:
    print(f"cluster docs contract: FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def enum_values(text: str, enum_name: str, prefix: str) -> list[tuple[int, str]]:
    match = re.search(
        rf"typedef\s+enum\s+{re.escape(enum_name)}\s*\{{(.*?)\}}\s*[^;]+;",
        text,
        re.S,
    )
    if not match:
        fail(f"missing enum {enum_name}")
    values = []
    for name, value in re.findall(
        rf"{re.escape(prefix)}([A-Z0-9_]+)\s*=\s*(\d+)", match.group(1)
    ):
        if name != "COUNT":
            values.append((int(value), name))
    if not values:
        fail(f"empty enum {enum_name}")
    return values


def section_rows(text: str, heading: str, next_heading: str) -> list[tuple[int, str]]:
    match = re.search(
        rf"##\s+{re.escape(heading)}\s*(.*?)(?=##\s+{re.escape(next_heading)})",
        text,
        re.S,
    )
    if not match:
        fail(f"missing document section {heading}")
    return [
        (int(value), name)
        for value, name in re.findall(
            r"^\|\s*(\d+)\s*\|\s*([A-Z][A-Z0-9_]+)\s*\|\s*$",
            match.group(1),
            re.M,
        )
    ]


def require(pattern: str, text: str, label: str) -> None:
    if not re.search(pattern, text, re.M):
        fail(f"{label} mismatch")


def main() -> None:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    cluster_h = (root / "include/ucn/ucn_cluster.h").read_text(encoding="utf-8")
    wire_h = (root / "include/ucn/ucn_cluster_wire_v4.h").read_text(encoding="utf-8")
    storage_h = (root / "include/ucn/ucn_cluster_storage.h").read_text(encoding="utf-8")
    persist_h = (root / "include/ucn/ucn_cluster_persist.h").read_text(encoding="utf-8")
    contract = (root / "docs/UCN_Cluster_Code_Doc_Contract.md").read_text(encoding="utf-8")
    current = (root / "docs/UCN_V5_Cluster_CURRENT_FSM.md").read_text(encoding="utf-8")
    target = (root / "docs/UCN_V5_Cluster_FSM_Design_v2.md").read_text(encoding="utf-8")
    rfc = (root / "docs/UCN_Cluster_Wire_v4.md").read_text(encoding="utf-8")
    tasks = (root / "docs/UCN_V5_Cluster_Current_to_Target_v2_详细修改方案与任务表.md").read_text(encoding="utf-8")

    phases = enum_values(cluster_h, "ucn_cluster_phase", "UCN_CLUSTER_PHASE_")
    wire_types = enum_values(wire_h, "ucn_cluster_wire_v4_type", "UCN_CLUSTER_WIRE_V4_MSG_")
    if phases != section_rows(contract, "Phase", "Wire v4 Type"):
        fail("Phase table differs from ucn_cluster_phase_t")
    if wire_types != section_rows(contract, "Wire v4 Type", "版本常量"):
        fail("Wire v4 Type table differs from public enum")

    for _, name in phases:
        if f"`{name}`" not in current:
            fail(f"CURRENT FSM omits Phase {name}")
    for value, name in wire_types:
        if not re.search(rf"\|\s*{value}\s+`?{name}`?\s*\|", rfc):
            fail(f"RFC4 omits Type {value} {name}")

    require(r"UCN_CLUSTER_API_VERSION\s+\(\(uint16_t\)2U\)", cluster_h, "API v2")
    require(r"UCN_CLUSTER_FORMAT_VERSION\s+\(\(uint8_t\)3U\)", cluster_h, "production format v3")
    require(r"UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES\s+\(\(size_t\)32U\)", cluster_h, "v3 bytes")
    require(r"UCN_CLUSTER_STORAGE_LAYOUT_VERSION\s+UINT32_C\(2\)", storage_h, "storage v2")
    require(r"UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES\s+\(\(size_t\)40U\)", wire_h, "v4 bytes")
    require(r"UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION\s+\\\s*\n\s*UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4", persist_h, "record schema v4")
    require(r"UCN_CLUSTER_PERSIST_RECORD_BYTES\s+\(\(size_t\)388U\)", persist_h, "record bytes")

    required_tokens = [
        "| Cluster API | 2 |",
        "| Storage layout | 2 |",
        "| production Cluster format | 3 |",
        "| recommended Cluster format | 4 |",
        "| Wire v3 bytes | 32 |",
        "| Wire v4 bytes | 40 |",
        "| Persistence writer schema | 4 |",
        "| Persistence record bytes | 388 |",
        "production Wire v4 RX/TX/FSM/Authority：`AUDIT HOLD`",
    ]
    for token in required_tokens:
        if token not in contract:
            fail(f"contract omits {token}")
    if "2026-08-25 实现对齐说明" not in target:
        fail("Target FSM lacks implementation-boundary notice")
    for number in range(1, 13):
        if f"CLV2-14-{number:02d}" not in tasks:
            fail(f"task table omits CLV2-14-{number:02d}")

    print(
        "cluster docs contract: PASSED "
        f"phases={len(phases)} wire_types={len(wire_types)} tasks=12"
    )


if __name__ == "__main__":
    main()
