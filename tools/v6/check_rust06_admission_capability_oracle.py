#!/usr/bin/env python3
"""Independently reconstruct RUST-06 Admission/Capability byte fixtures."""

from __future__ import annotations

import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "rust" / "tests" / "conformance" / "v6s_admission_capability_v1.h"


def fixture(name: str) -> bytes:
    text = FIXTURE.read_text(encoding="utf-8")
    match = re.search(
        rf"#define\s+{re.escape(name)}\s+(.*?)(?=\n\s*#(?:define|endif))",
        text,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing fixture: {name}")
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))


def crc32c(value: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in value:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def main() -> None:
    identity = bytes([0x11]) * 16
    device_nonce = 0x0102030405060708
    transaction_id = 0x1112131415161718
    hello = (
        bytes((1, 1, 0, 0))
        + identity
        + struct.pack(">QQ", device_nonce, transaction_id)
        + bytes(4)
    )
    challenge = (
        bytes((1, 1, 4, 0))
        + struct.pack(">QI", transaction_id, 0x01020304)
        + bytes((1, 2, 3, 4))
        + bytes(20)
    )
    hello_cookie = (
        bytes((1, 1, 4, 0))
        + identity
        + struct.pack(">QQQHI", device_nonce, transaction_id, 0x2122232425262728, 7, 9)
        + bytes([0x31]) * 32
        + bytes((0xC1, 0xC2, 0xC3, 0xC4))
    )
    capability = struct.pack(
        ">IIII I HHHHHH II III HHBB HHHI".replace(" ", ""),
        0x01020304,
        0x11121314,
        1500,
        1400,
        1300,
        14,
        2,
        4,
        12,
        8,
        12,
        1_000_000,
        0,
        0x0000010B,
        2,
        6,
        16,
        2,
        4,
        4,
        0,
        0,
        0,
        0,
    )
    digest = b"".join(
        struct.pack(">I", crc32c(bytes((0xA5 + index * 0x17,)) + capability))
        for index in range(4)
    )
    summary = struct.pack(">II", 0x01020304, 0x11121314) + digest
    query = struct.pack(">I", 0x01020304) + digest

    checks = {
        "UCN_V6S_ADMISSION_HELLO_JOIN_BYTES": hello,
        "UCN_V6S_ADMISSION_COOKIE_CHALLENGE_BYTES": challenge,
        "UCN_V6S_ADMISSION_HELLO_COOKIE_BYTES": hello_cookie,
        "UCN_V6S_CAPABILITY_RECORD_BYTES": capability,
        "UCN_V6S_CAPABILITY_DIGEST_BYTES": digest,
        "UCN_V6S_CAPABILITY_SUMMARY_BYTES": summary,
        "UCN_V6S_CAPABILITY_QUERY_BYTES": query,
    }
    for name, expected in checks.items():
        actual = fixture(name)
        if actual != expected:
            raise AssertionError(
                f"{name} mismatch\nfixture={actual.hex().upper()}\noracle ={expected.hex().upper()}"
            )
    print(f"RUST06_WIRE_ORACLE_OK checks={len(checks)}")


if __name__ == "__main__":
    main()
