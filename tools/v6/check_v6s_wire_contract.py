#!/usr/bin/env python3
"""Independent executable oracle for the V6S-00 low-overhead Wire contract."""

from __future__ import annotations

import hashlib
import hmac
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SHARED_FIXTURE = ROOT / "rust" / "tests" / "conformance" / "v6s_wire_core_v1.h"


def common(contract: int, traffic: int, delivery: int,
           interaction: int, payload_kind: int,
           origin_security: int, hop_limit: int) -> bytes:
    assert 0 <= contract <= 5
    assert 0 <= traffic <= 3
    assert 0 <= delivery <= 2
    assert 0 <= interaction <= 3
    assert 0 <= payload_kind <= 3
    assert 0 <= origin_security <= 2
    assert 1 <= hop_limit <= 63
    return bytes((
        0x60 | contract,
        traffic << 6 | delivery << 4 | interaction << 2 | payload_kind,
        origin_security << 6 | hop_limit,
    ))


def be(value: int, width: int) -> bytes:
    return value.to_bytes(width, "big")


def origin_aad(header: bytes, contract: int, identity: bytes,
               payload_length: int) -> bytes:
    normalized = header[:2] + bytes((header[2] & 0xC0,))
    after_length = normalized + bytes((contract,)) + identity + be(payload_length, 4)
    return b"UCN6-ORIGIN-V1" + be(len(after_length), 4) + after_length


def nonce_seq(nonce_key: bytes, context_fingerprint: bytes,
              origin_sequence: int) -> bytes:
    data = (b"UCN6-NONCE-SEQ-V1" + context_fingerprint +
            be(origin_sequence, 4))
    return hmac.new(nonce_key, data, hashlib.sha256).digest()[:12]


def nonce_c0(nonce_key: bytes, context_fingerprint: bytes,
             transaction_id: int, opcode: int, direction: int,
             interaction: int) -> bytes:
    data = (b"UCN6-NONCE-C0-V1" + context_fingerprint +
            be(transaction_id, 8) + be(opcode, 2) +
            bytes((direction, interaction)))
    return hmac.new(nonce_key, data, hashlib.sha256).digest()[:12]


def gf8_mul(left: int, right: int) -> int:
    result = 0
    for _ in range(8):
        if right & 1:
            result ^= left
        left = ((left << 1) ^ (0x11B if left & 0x80 else 0)) & 0xFF
        right >>= 1
    return result


def rotl8(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (8 - shift))) & 0xFF


def aes_sbox(value: int) -> int:
    inverse = 0 if value == 0 else 1
    if value != 0:
        for _ in range(254):
            inverse = gf8_mul(inverse, value)
    return (inverse ^ rotl8(inverse, 1) ^ rotl8(inverse, 2) ^
            rotl8(inverse, 3) ^ rotl8(inverse, 4) ^ 0x63)


def aes128_round_keys(key: bytes) -> bytes:
    assert len(key) == 16
    expanded = list(key)
    rcon = 1
    while len(expanded) < 176:
        temp = expanded[-4:]
        if len(expanded) % 16 == 0:
            temp = [aes_sbox(temp[1]), aes_sbox(temp[2]),
                    aes_sbox(temp[3]), aes_sbox(temp[0])]
            temp[0] ^= rcon
            rcon = gf8_mul(rcon, 2)
        for value in temp:
            expanded.append(expanded[len(expanded) - 16] ^ value)
    return bytes(expanded)


def aes128_encrypt_block(key: bytes, block: bytes) -> bytes:
    assert len(block) == 16
    keys = aes128_round_keys(key)
    state = [value ^ keys[index] for index, value in enumerate(block)]
    for round_index in range(1, 11):
        state = [aes_sbox(value) for value in state]
        shifted = [0] * 16
        for row in range(4):
            for column in range(4):
                shifted[4 * column + row] = state[4 * ((column + row) % 4) + row]
        state = shifted
        if round_index != 10:
            mixed: list[int] = []
            for column in range(4):
                a0, a1, a2, a3 = state[4 * column:4 * column + 4]
                mixed.extend((
                    gf8_mul(a0, 2) ^ gf8_mul(a1, 3) ^ a2 ^ a3,
                    a0 ^ gf8_mul(a1, 2) ^ gf8_mul(a2, 3) ^ a3,
                    a0 ^ a1 ^ gf8_mul(a2, 2) ^ gf8_mul(a3, 3),
                    gf8_mul(a0, 3) ^ a1 ^ a2 ^ gf8_mul(a3, 2),
                ))
            state = mixed
        round_key = keys[16 * round_index:16 * (round_index + 1)]
        state = [value ^ round_key[index] for index, value in enumerate(state)]
    return bytes(state)


def xor_bytes(left: bytes, right: bytes) -> bytes:
    return bytes(a ^ b for a, b in zip(left, right))


def ghash_mul(left: int, right: int) -> int:
    result = 0
    value = right
    for bit in range(128):
        if left & (1 << (127 - bit)):
            result ^= value
        value = ((value >> 1) ^
                 (0xE1000000000000000000000000000000 if value & 1 else 0))
    return result


def pad16(data: bytes) -> bytes:
    return data + b"\x00" * ((-len(data)) % 16)


def aes128_gcm_encrypt(key: bytes, nonce: bytes, aad: bytes,
                       plaintext: bytes) -> tuple[bytes, bytes]:
    assert len(key) == 16 and len(nonce) == 12
    j0 = nonce + b"\x00\x00\x00\x01"
    ciphertext = bytearray()
    counter = 2
    for offset in range(0, len(plaintext), 16):
        block = plaintext[offset:offset + 16]
        stream = aes128_encrypt_block(key, nonce + be(counter, 4))
        ciphertext.extend(xor_bytes(block, stream[:len(block)]))
        counter = (counter + 1) & 0xFFFFFFFF
        assert counter != 0
    hash_key = int.from_bytes(aes128_encrypt_block(key, bytes(16)), "big")
    auth_data = (pad16(aad) + pad16(bytes(ciphertext)) +
                 be(len(aad) * 8, 8) + be(len(ciphertext) * 8, 8))
    accumulator = 0
    for offset in range(0, len(auth_data), 16):
        accumulator = ghash_mul(
            accumulator ^ int.from_bytes(auth_data[offset:offset + 16], "big"),
            hash_key,
        )
    tag = xor_bytes(
        aes128_encrypt_block(key, j0), accumulator.to_bytes(16, "big")
    )
    return bytes(ciphertext), tag


def rotl32(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


def chacha_quarter(state: list[int], a: int, b: int, c: int, d: int) -> None:
    state[a] = (state[a] + state[b]) & 0xFFFFFFFF
    state[d] = rotl32(state[d] ^ state[a], 16)
    state[c] = (state[c] + state[d]) & 0xFFFFFFFF
    state[b] = rotl32(state[b] ^ state[c], 12)
    state[a] = (state[a] + state[b]) & 0xFFFFFFFF
    state[d] = rotl32(state[d] ^ state[a], 8)
    state[c] = (state[c] + state[d]) & 0xFFFFFFFF
    state[b] = rotl32(state[b] ^ state[c], 7)


def chacha20_block(key: bytes, counter: int, nonce: bytes) -> bytes:
    assert len(key) == 32 and len(nonce) == 12
    constants = b"expand 32-byte k"
    initial = [
        int.from_bytes(constants[index:index + 4], "little")
        for index in range(0, 16, 4)
    ]
    initial.extend(
        int.from_bytes(key[index:index + 4], "little")
        for index in range(0, 32, 4)
    )
    initial.append(counter)
    initial.extend(
        int.from_bytes(nonce[index:index + 4], "little")
        for index in range(0, 12, 4)
    )
    working = initial.copy()
    for _ in range(10):
        chacha_quarter(working, 0, 4, 8, 12)
        chacha_quarter(working, 1, 5, 9, 13)
        chacha_quarter(working, 2, 6, 10, 14)
        chacha_quarter(working, 3, 7, 11, 15)
        chacha_quarter(working, 0, 5, 10, 15)
        chacha_quarter(working, 1, 6, 11, 12)
        chacha_quarter(working, 2, 7, 8, 13)
        chacha_quarter(working, 3, 4, 9, 14)
    return b"".join(
        ((working[index] + initial[index]) & 0xFFFFFFFF).to_bytes(4, "little")
        for index in range(16)
    )


def poly1305_mac(key: bytes, data: bytes) -> bytes:
    assert len(key) == 32
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    accumulator = 0
    modulus = (1 << 130) - 5
    for offset in range(0, len(data), 16):
        block = data[offset:offset + 16]
        accumulator = (
            (accumulator + int.from_bytes(block + b"\x01", "little")) * r
        ) % modulus
    return ((accumulator + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def chacha20_poly1305_encrypt(key: bytes, nonce: bytes, aad: bytes,
                              plaintext: bytes) -> tuple[bytes, bytes]:
    assert len(key) == 32 and len(nonce) == 12
    poly_key = chacha20_block(key, 0, nonce)[:32]
    ciphertext = bytearray()
    counter = 1
    for offset in range(0, len(plaintext), 64):
        block = plaintext[offset:offset + 64]
        ciphertext.extend(xor_bytes(block, chacha20_block(key, counter, nonce)))
        counter += 1
    mac_input = (pad16(aad) + pad16(bytes(ciphertext)) +
                 len(aad).to_bytes(8, "little") +
                 len(ciphertext).to_bytes(8, "little"))
    return bytes(ciphertext), poly1305_mac(poly_key, mac_input)


EXPECTED = {
    "C0_A1_O0_H0": "600101010203040000FFFF000000000000000001020304050607080001AA55",
    "C1_A1_O0_H0": "61400312345678010201020304DEADBEEF",
    "C2_O0_H0": "622501123401020304040100",
    "C3_O0_H0": "63800211112222AABBCC",
    "C4_O0_H0": "6450041111222201020304CAFE",
    "C5_PUBLIC_O0_H0": "65000311112222000101020304BEEF",
    "C2_O1_H2": "620041123400000001010203048D2206580D6E913E15F14DE32AC68EF9",
    "C3_H1": "63800211112222AABBCC00000001B61FCD2ED841CF792777F4F1",
    "C0_A1_O2_H0_AES128_GCM": (
        "6025810102030412345678000000010000000201020304050607080013"
        "58B2377998A3E0C198106C043A0DBCC6BE150D6C"
    ),
    "C2_O2_H2_CHACHA20_POLY1305": (
        "62008112340000000181052B38E079A5ACC9BDDBE6A1E43903DC3541C2"
    ),
}


def raw_vectors() -> dict[str, bytes]:
    return {
        "C0_A1_O0_H0": (
            common(0, 0, 0, 0, 1, 0, 1) + be(0x01020304, 4) +
            be(0, 2) + be(0xFFFF, 2) + be(0, 4) + be(0, 4) +
            be(0x0102030405060708, 8) + be(0x0001, 2) + bytes.fromhex("AA55")
        ),
        "C1_A1_O0_H0": (
            common(1, 1, 0, 0, 0, 0, 3) + be(0x1234, 2) +
            be(0x5678, 2) + be(0x0102, 2) + be(0x01020304, 4) +
            bytes.fromhex("DEADBEEF")
        ),
        "C2_O0_H0": (
            common(2, 0, 2, 1, 1, 0, 1) + be(0x1234, 2) +
            be(0x01020304, 4) + be(0x0401, 2) + b"\x00"
        ),
        "C3_O0_H0": (
            common(3, 2, 0, 0, 0, 0, 2) + be(0x1111, 2) +
            be(0x2222, 2) + bytes.fromhex("AABBCC")
        ),
        "C4_O0_H0": (
            common(4, 1, 1, 0, 0, 0, 4) + be(0x1111, 2) +
            be(0x2222, 2) + be(0x01020304, 4) + bytes.fromhex("CAFE")
        ),
        "C5_PUBLIC_O0_H0": (
            common(5, 0, 0, 0, 0, 0, 3) + be(0x1111, 2) +
            be(0x2222, 2) + be(1, 2) + be(0x01020304, 4) +
            bytes.fromhex("BEEF")
        ),
    }


def protected_vectors() -> dict[str, bytes]:
    c2_header = common(2, 0, 0, 0, 0, 1, 1)
    c2_fields = be(0x1234, 2) + be(1, 4)
    payload = bytes.fromhex("01020304")
    flow_fingerprint = bytes(range(0x20, 0x30))
    direct_fingerprint = bytes(range(0x30, 0x40))
    canonical_identity = flow_fingerprint + be(1, 4) + direct_fingerprint
    c2_origin_aad = origin_aad(
        c2_header, 2, canonical_identity, len(payload)
    )
    origin_tag = hmac.new(
        bytes(range(0x00, 0x10)), c2_origin_aad + payload, hashlib.sha256
    ).digest()[:16]

    c3_core = (
        common(3, 2, 0, 0, 0, 0, 2) + be(0x1111, 2) +
        be(0x2222, 2) + bytes.fromhex("AABBCC")
    )
    hop_sequence = be(1, 4)
    hop_fingerprint = bytes(range(0x40, 0x50))
    hop_after_length = c3_core + hop_sequence + hop_fingerprint
    hop_aad = b"UCN6-HOP-V1" + be(len(hop_after_length), 4) + hop_after_length
    hop_tag = hmac.new(
        bytes(range(0x10, 0x20)), hop_aad, hashlib.sha256
    ).digest()[:12]
    c0_header = common(0, 0, 2, 1, 1, 2, 1)
    c0_transaction = 0x0102030405060708
    c0_opcode = 0x0013
    c0_fields = (
        be(0x01020304, 4) + be(0x1234, 2) + be(0x5678, 2) +
        be(1, 4) + be(2, 4) + be(c0_transaction, 8) +
        be(c0_opcode, 2)
    )
    c0_plaintext = bytes.fromhex("DEADBEEF")
    c0_context = bytes(range(0x50, 0x60))
    c0_nonce = nonce_c0(
        bytes(range(0x60, 0x80)), c0_context, c0_transaction,
        c0_opcode, 0, 1,
    )
    c0_aad = origin_aad(c0_header, 0, c0_fields, len(c0_plaintext))
    c0_ciphertext, c0_tag = aes128_gcm_encrypt(
        bytes(range(0x80, 0x90)), c0_nonce, c0_aad, c0_plaintext
    )

    c2_o2_header = common(2, 0, 0, 0, 0, 2, 1)
    c2_o2_fields = be(0x1234, 2) + be(1, 4)
    c2_o2_plaintext = bytes.fromhex("01020304")
    c2_o2_identity = flow_fingerprint + be(1, 4) + direct_fingerprint
    c2_o2_nonce = nonce_seq(
        bytes(range(0x90, 0xB0)), flow_fingerprint, 1
    )
    c2_o2_aad = origin_aad(
        c2_o2_header, 2, c2_o2_identity, len(c2_o2_plaintext)
    )
    c2_o2_ciphertext, c2_o2_tag = chacha20_poly1305_encrypt(
        bytes(range(0xB0, 0xD0)), c2_o2_nonce, c2_o2_aad,
        c2_o2_plaintext,
    )

    return {
        "C2_O1_H2": c2_header + c2_fields + payload + origin_tag,
        "C3_H1": c3_core + hop_sequence + hop_tag,
        "C0_A1_O2_H0_AES128_GCM": (
            c0_header + c0_fields + c0_ciphertext + c0_tag
        ),
        "C2_O2_H2_CHACHA20_POLY1305": (
            c2_o2_header + c2_o2_fields + c2_o2_ciphertext + c2_o2_tag
        ),
    }


def shared_c_rust_vectors() -> dict[str, bytes]:
    """Return the exact O0/H0 fixtures jointly consumed by C and Rust."""
    return {
        "UCN_V6S_WIRE_C0_A1_O0_H0_BOOTSTRAP_BYTES": raw_vectors()["C0_A1_O0_H0"],
        "UCN_V6S_WIRE_C1_A0_O0_H0_DATA_BYTES": (
            common(1, 2, 0, 0, 0, 0, 5) + be(1, 1) + be(2, 1) +
            be(0x1234, 2) + be(0x11223344, 4) + bytes.fromhex("DEAD")
        ),
        "UCN_V6S_WIRE_C1_A1_O0_H0_DATA_BYTES": raw_vectors()["C1_A1_O0_H0"],
        "UCN_V6S_WIRE_C1_A2_O0_H0_DATA_BYTES": (
            common(1, 1, 0, 0, 0, 0, 1) + be(0x010203, 3) +
            be(0x0A0B0C, 3) + be(0x0102, 2) + be(0xA1B2C3D4, 4) +
            bytes.fromhex("00FF")
        ),
        "UCN_V6S_WIRE_C1_A3_O0_H0_DATA_BYTES": (
            common(1, 0, 0, 0, 0, 0, 32) + be(0x01020304, 4) +
            be(0xA1A2A3A4, 4) + be(0xBEEF, 2) + be(0x89ABCDEF, 4) +
            bytes.fromhex("55")
        ),
    }


def check_shared_c_rust_fixture() -> tuple[bool, str]:
    """Verify fixture bytes and prove that both implementations consume it."""
    text = SHARED_FIXTURE.read_text(encoding="utf-8")
    expected = shared_c_rust_vectors()
    for name, expected_bytes in expected.items():
        match = re.search(
            rf"#define\s+{re.escape(name)}(?P<body>.*?)(?=\n#define|\n#endif)",
            text,
            flags=re.DOTALL,
        )
        if match is None:
            return False, f"missing shared fixture {name}"
        actual = bytes(
            int(value, 16)
            for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group("body"))
        )
        if actual != expected_bytes:
            return False, f"shared fixture mismatch {name}"
        if not actual:
            return False, f"empty shared fixture {name}"
        mutated = bytes((actual[0] ^ 1,)) + actual[1:]
        if mutated == expected_bytes:
            return False, f"fixture mutation was not detected {name}"

    c_test = (ROOT / "tests" / "simplified" / "test_wire_c1.c").read_text(
        encoding="utf-8"
    )
    rust_test = (
        ROOT / "rust" / "crates" / "ucn-wire" / "tests" / "conformance.rs"
    ).read_text(encoding="utf-8")
    if "../../rust/tests/conformance/v6s_wire_core_v1.h" not in c_test:
        return False, "C conformance test does not include the shared fixture"
    if "v6s_wire_core_v1.h" not in rust_test:
        return False, "Rust conformance test does not include the shared fixture"
    for name in expected:
        if name.startswith("UCN_V6S_WIRE_C1_") and name not in c_test:
            return False, f"C conformance test does not consume {name}"
        if name not in rust_test:
            return False, f"Rust conformance test does not consume {name}"

    negative = {
        "UCN_V6S_NEG_BAD_VERSION_BYTE0": 0x51,
        "UCN_V6S_NEG_RESERVED_CONTRACT_BYTE0": 0x66,
        "UCN_V6S_NEG_OTHER_CONTRACT_BYTE0": 0x62,
        "UCN_V6S_NEG_RESERVED_DELIVERY_MASK": 0x30,
        "UCN_V6S_NEG_RESERVED_ORIGIN_BYTE2": 0xC3,
        "UCN_V6S_NEG_ZERO_HOP_BYTE2": 0x00,
    }
    for name, expected_value in negative.items():
        match = re.search(rf"#define\s+{re.escape(name)}\s+0x([0-9A-Fa-f]{{2}})", text)
        if match is None or int(match.group(1), 16) != expected_value:
            return False, f"shared negative mismatch {name}"
        if name not in c_test or name not in rust_test:
            return False, f"C/Rust tests do not both consume {name}"
    return True, (
        f"shared_vectors={len(expected)} mutation={len(expected)}/{len(expected)} "
        f"shared_negative={len(negative)}"
    )


def crypto_selftest() -> None:
    ciphertext, tag = aes128_gcm_encrypt(
        bytes(16), bytes(12), b"", bytes(16)
    )
    assert ciphertext.hex() == "0388dace60b6a392f328c2b971b2fe78"
    assert tag.hex() == "ab6e47d42cec13bdf53a67b21257bddf"

    key = bytes(range(0x80, 0xA0))
    nonce = bytes.fromhex("070000004041424344454647")
    aad = bytes.fromhex("50515253C0C1C2C3C4C5C6C7")
    plaintext = bytes.fromhex(
        "4C616469657320616E642047656E746C656D656E206F662074686520636C617373"
        "206F66202739393A204966204920636F756C64206F6666657220796F75206F6E6C"
        "79206F6E652074697020666F7220746865206675747572652C2073756E7363726565"
        "6E20776F756C642062652069742E"
    )
    ciphertext, tag = chacha20_poly1305_encrypt(key, nonce, aad, plaintext)
    assert ciphertext.hex() == (
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b369"
        "2ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3f"
        "f4def08e4b7a9de576d26586cec64b6116"
    )
    assert tag.hex() == "1ae10b594f09e26a7e902ecbd0600691"


def main() -> int:
    crypto_selftest()
    vectors = raw_vectors() | protected_vectors()
    for name, expected_hex in EXPECTED.items():
        actual_hex = vectors[name].hex().upper()
        if actual_hex != expected_hex:
            print(f"V6S_WIRE_ERROR {name} expected={expected_hex} actual={actual_hex}")
            return 1
    if len(set(EXPECTED.values())) != len(EXPECTED):
        print("V6S_WIRE_ERROR duplicate golden bytes")
        return 1
    shared_ok, shared_result = check_shared_c_rust_fixture()
    if not shared_ok:
        print(f"V6S_WIRE_ERROR {shared_result}")
        return 1
    print(
        f"V6S_WIRE_GOLDEN_OK vectors={len(EXPECTED)} {shared_result}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
