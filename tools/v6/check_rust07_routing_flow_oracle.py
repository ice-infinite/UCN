#!/usr/bin/env python3
"""Independently reconstruct RUST-07 Routing/Flow byte fixtures."""

from __future__ import annotations

import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "rust" / "tests" / "conformance" / "v6s_routing_flow_v1.h"


def fixture(name: str) -> bytes:
    text = FIXTURE.read_text(encoding="utf-8")
    match = re.search(
        rf"#define\s+{re.escape(name)}\s+(.*?)(?=\n\s*#(?:define|endif))",
        text,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing fixture: {name}")
    return bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1))
    )


def common_header(contract: int, delivery: int, interaction: int, kind: int, origin: int) -> bytes:
    version = 6
    traffic_class = 1
    hop_limit = 7
    return bytes(
        (
            (version << 4) | contract,
            (traffic_class << 6) | (delivery << 4) | (interaction << 2) | kind,
            (origin << 6) | hop_limit,
        )
    )


def main() -> None:
    rreq = struct.pack(">IHHB", 0x01020304, 0x1122, 0x3344, 0x03)
    rrep = bytes([0xA5]) * 16 + struct.pack(">IBIHH", 0x01020304, 0, 0x11223344, 0x5566, 0x7788)
    rerr = b"".join(
        (
            struct.pack(">I", 0x01020304),
            bytes([0x11]) * 16,
            struct.pack(">III", 0x11223344, 0x01020304, 0x05060708),
            bytes([0x22]) * 16,
            struct.pack(">IIIQ", 0x55667788, 0x11121314, 0x21222324, 0x3132333435363738),
            bytes([0x33]) * 16,
            struct.pack(">IIHIB", 0x99AABBCC, 0x41424344, 0x5152, 0x61626364, 4),
        )
    )
    label_setup = struct.pack(">HIHHHI", 0x1122, 0x33445566, 0x7788, 0x99AA, 0xBBCC, 0xDDEEF001)
    c2 = common_header(2, 2, 1, 1, 1) + struct.pack(">HI", 0x1122, 0x33445566)
    c3 = common_header(3, 0, 0, 0, 0) + struct.pack(">HH", 0x1122, 0x3344)
    c4 = common_header(4, 2, 1, 1, 1) + struct.pack(">HHI", 0x1122, 0x3344, 0x55667788)
    checks = {
        "UCN_V6S_ROUTE_RREQ_BYTES": rreq,
        "UCN_V6S_ROUTE_RREP_BYTES": rrep,
        "UCN_V6S_ROUTE_RERR_BYTES": rerr,
        "UCN_V6S_FLOW_LABEL_SETUP_BYTES": label_setup,
        "UCN_V6S_FLOW_C2_PREFIX_BYTES": c2,
        "UCN_V6S_FLOW_C3_PREFIX_BYTES": c3,
        "UCN_V6S_FLOW_C4_PREFIX_BYTES": c4,
    }
    for name, expected in checks.items():
        actual = fixture(name)
        if actual != expected:
            raise AssertionError(
                f"{name} mismatch\nfixture={actual.hex().upper()}\noracle ={expected.hex().upper()}"
            )
    print(f"RUST07_ROUTING_FLOW_ORACLE_OK checks={len(checks)}")


if __name__ == "__main__":
    main()
