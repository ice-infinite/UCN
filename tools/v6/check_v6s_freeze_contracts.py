#!/usr/bin/env python3
"""Fail closed when the V6S-00 contract-freeze candidate drifts."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")


CONTRACTS = (
    "00-冻结范围权威关系与签字规则.md",
    "01-文档结构与机器门禁.md",
    "02-基础标量Generation与Registry命名空间.md",
    "03-Core-Wire精确布局与安全覆盖.md",
    "04-Golden-Negative-Fuzz与属性测试合同.md",
    "05-公共API-ABI与Feature-OFF合同.md",
    "06-Profile容量公式与资源门禁.md",
    "07-Persistence-Foundation共同合同.md",
    "08-跨合同追踪与最终自审.md",
)
README_NAME = "README.md"
ALLOWED_TEST_REMEDIATIONS = {
    "tests/v6/test_v6_identity.c",
    "tests/v6/test_v6_security.c",
}
ALLOWED_PRODUCTION_REMEDIATIONS = {
    "src/v6/identity/ucn_v6_bootstrap.c",
    "src/v6/runtime/ucn_v6_runtime.c",
    "src/v6/security/ucn_v6_security.c",
}
REMEDIATION_CONTENT_FILES = tuple(sorted(
    ALLOWED_TEST_REMEDIATIONS | ALLOWED_PRODUCTION_REMEDIATIONS
))
MANIFEST_RELATIVE = "docs/09-审计与整改/UCN_V6_简化文档候选清单.sha256"


def require(text: str, token: str, label: str, errors: list[str]) -> None:
    if token not in text:
        errors.append(f"{label}: missing {token!r}")


def validate_readme_gate(readme: str) -> list[str]:
    """Validate the implementation-release status independently of file loading."""
    errors: list[str] = []
    require(readme, "EXTERNAL REVIEW REQUIRED / AUDIT HOLD", "README", errors)
    require(readme, "IMPL-00 = BLOCKED", "README", errors)
    if "IMPL-00 = AUTHORIZED" in readme or "IMPL-00 = GO" in readme:
        errors.append("README: implementation is authorized before external review")
    return errors


def selftest_readme_gate() -> list[str]:
    """Prove that the gate rejects the exact premature-release mutation."""
    errors: list[str] = []
    valid = "EXTERNAL REVIEW REQUIRED / AUDIT HOLD\nIMPL-00 = BLOCKED\n"
    if validate_readme_gate(valid):
        errors.append("README gate self-test rejected the valid HOLD fixture")
    mutated = valid.replace("IMPL-00 = BLOCKED", "IMPL-00 = AUTHORIZED")
    if not validate_readme_gate(mutated):
        errors.append("README gate self-test accepted premature IMPL-00 authorization")
    return errors


def parse_manifest(text: str) -> tuple[dict[str, tuple[str, int]], list[str]]:
    """Parse the generated candidate manifest without trusting its producer."""
    entries: dict[str, tuple[str, int]] = {}
    errors: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("  ", 2)
        if (len(parts) != 3 or
                re.fullmatch(r"[0-9A-F]{64}", parts[0]) is None or
                not parts[1].isdigit() or not parts[2]):
            errors.append(f"candidate manifest: malformed line {line_number}")
            continue
        path = parts[2]
        if path in entries:
            errors.append(f"candidate manifest: duplicate path {path}")
            continue
        entries[path] = (parts[0], int(parts[1]))
    return entries, errors


def manifest_entry_matches(entry: tuple[str, int] | None, data: bytes) -> bool:
    return (entry is not None and entry[1] == len(data) and
            entry[0] == hashlib.sha256(data).hexdigest().upper())


def validate_remediation_manifest(root: Path, manifest: str) -> list[str]:
    """Require exact bytes for every path allowed to change production/tests."""
    entries, errors = parse_manifest(manifest)
    for relative in REMEDIATION_CONTENT_FILES:
        path = root / relative
        if not path.is_file():
            errors.append(f"candidate manifest: missing remediation file {relative}")
            continue
        if not manifest_entry_matches(entries.get(relative), path.read_bytes()):
            errors.append(
                f"candidate manifest: stale/missing remediation entry {relative}"
            )
    return errors


def selftest_remediation_manifest(root: Path, manifest: str) -> list[str]:
    """Prove a same-size one-byte change cannot pass with the old manifest."""
    entries, parse_errors = parse_manifest(manifest)
    if parse_errors:
        return ["candidate manifest self-test cannot parse valid fixture"]
    relative = REMEDIATION_CONTENT_FILES[0]
    data = (root / relative).read_bytes()
    entry = entries.get(relative)
    if not data or not manifest_entry_matches(entry, data):
        return ["candidate manifest self-test rejected current remediation bytes"]
    mutated = bytearray(data)
    mutated[len(mutated) // 2] ^= 1
    if manifest_entry_matches(entry, bytes(mutated)):
        return ["candidate manifest self-test accepted one-byte remediation drift"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    errors: list[str] = []

    contract_dir = (
        root / "docs" / "10-理论与规划" / "建议方案" /
        "UCN_v6_V6S_00_简化版实施合同冻结"
    )
    texts: dict[str, str] = {}
    for name in CONTRACTS:
        path = contract_dir / name
        if not path.is_file():
            errors.append(f"missing contract: {path.relative_to(root)}")
            continue
        text = path.read_text(encoding="utf-8")
        texts[name] = text
        if name != CONTRACTS[-1]:
            require(text, "DONE / SELF-REVIEW PASS", name, errors)
        for stale in ("TODO", "TBD", "待补", "PLACEHOLDER"):
            if stale in text:
                errors.append(f"{name}: unfinished marker {stale!r}")
    readme_path = contract_dir / README_NAME
    if not readme_path.is_file():
        errors.append(f"missing contract entrypoint: {readme_path.relative_to(root)}")
    else:
        texts[README_NAME] = readme_path.read_text(encoding="utf-8")

    scalar = texts.get(CONTRACTS[2], "")
    for token in (
        "C0TransactionId", "64", "big-endian", "no-wrap",
        "Protocol Opcode", "Address Binding | 32",
    ):
        require(scalar, token, CONTRACTS[2], errors)

    wire = texts.get(CONTRACTS[3], "")
    for token in (
        "共同 3 B Header", "基础头为 `25+2W`", "基础头为 `9+2W`",
        "基础头 9 B", "基础头 7 B", "基础头 11 B", "基础头 13 B",
        "O1 HMAC-SHA-256-128", "H1/H3 HMAC-SHA-256-96",
        "Core Packet 不携带 Magic、总 Length 或通用 CRC",
        "C0 | BestEffort/Reliable", "C5 | BestEffort/Latest/Reliable",
        "Protocol Opcode/Subtype 的唯一位置", "UCN6-NONCE-C0-V1",
        "byte2 & 0xC0",
    ):
        require(wire, token, CONTRACTS[3], errors)
    opcode_values = re.findall(r"^\| `0x([0-9A-F]{4})` \| ([A-Z0-9_]+) \|", wire, re.M)
    if len(opcode_values) != 94:
        errors.append(f"wire registry: expected 94 opcodes, found {len(opcode_values)}")
    if len({value for value, _ in opcode_values}) != len(opcode_values):
        errors.append("wire registry: duplicate opcode value")
    if len({name for _, name in opcode_values}) != len(opcode_values):
        errors.append("wire registry: duplicate opcode name")

    tests = texts.get(CONTRACTS[4], "")
    for token in (
        "0x56300004", "16384", "完整不写回", "独立 Wire oracle",
        "十条初始 Golden", "AES-128-GCM", "ChaCha20-Poly1305",
        "current=TYPE_MAX -> checked_next=EXHAUSTED/FAULT",
    ):
        require(tests, token, CONTRACTS[4], errors)

    api = texts.get(CONTRACTS[5], "")
    for token in (
        "UCN_ERR_IN_DOUBT", "sizeof(ucn_handle_t)==12", "ucn_publish(",
        "ucn_request(", "ucn_respond(", "ucn_publish_group(",
        "UCN_DECLARE_STORAGE", "Feature OFF", "符号数必须为零",
        "typedef int32_t ucn_result_t", "-fshort-enums",
    ):
        require(api, token, CONTRACTS[5], errors)

    profile = texts.get(CONTRACTS[6], "")
    for token in (
        "Composition 与 Profile 正交", "`1..255`", "单个协议 C 函数静态栈上限",
        "256 B", "五本资源账", "ADAPTER_FRAME_BYTES", "CLUSTER_VOTERS",
    ):
        require(profile, token, CONTRACTS[6], errors)

    persistence = texts.get(CONTRACTS[7], "")
    for token in (
        "Envelope 固定 96 B", "Commit Marker", "16 B marker", "BLAKE2s-128",
        "Anti-rollback Witness", "witness 指向的最新槽损坏/缺失",
        "Foundation 只提供原子 Record", "VOLATILE_TEST", "schema_id",
        "Compiled Durable Manifest Digest", "begin_load_slot", "io_token",
    ):
        require(persistence, token, CONTRACTS[7], errors)

    task_path = root / "docs" / "00-项目管理" / "00-任务表.md"
    task = task_path.read_text(encoding="utf-8") if task_path.is_file() else ""
    for index in range(9):
        task_id = f"V6S-00-{index:02d}"
        lines = [line for line in task.splitlines() if line.startswith(f"| {task_id} |")]
        if len(lines) != 1:
            errors.append(f"task table: {task_id} occurs {len(lines)} times")
        elif "DONE / SELF-REVIEW PASS" not in lines[0]:
            errors.append(f"task table: {task_id} is not self-reviewed DONE")
    require(task, "IMPL_00                 = BLOCKED BY V6S-00-08 EXTERNAL GO",
            "task table", errors)
    require(task, "V6S_00_CONTRACT_FREEZE = SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED / AUDIT HOLD",
            "task table", errors)

    readme = texts.get(README_NAME, "")
    errors.extend(validate_readme_gate(readme))
    errors.extend(selftest_readme_gate())

    manifest_path = root / MANIFEST_RELATIVE
    if not manifest_path.is_file():
        errors.append(f"missing candidate manifest: {MANIFEST_RELATIVE}")
    else:
        manifest = manifest_path.read_text(encoding="utf-8")
        errors.extend(validate_remediation_manifest(root, manifest))
        errors.extend(selftest_remediation_manifest(root, manifest))
        manifest_generator = (
            root / "tools" / "v6" /
            "generate_v6_simplified_docs_manifest.py"
        )
        completed = subprocess.run(
            [sys.executable, str(manifest_generator), "--root", str(root),
             "--output", MANIFEST_RELATIVE, "--check"],
            cwd=root,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            errors.append(
                "candidate manifest full verification failed: " +
                (completed.stdout + completed.stderr).strip()
            )

    wire_oracle = root / "tools" / "v6" / "check_v6s_wire_contract.py"
    if not wire_oracle.is_file():
        errors.append("missing independent wire oracle")
    else:
        completed = subprocess.run(
            [sys.executable, str(wire_oracle)],
            cwd=root,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0 or "V6S_WIRE_GOLDEN_OK vectors=10" not in completed.stdout:
            errors.append(
                "wire oracle failed: " + (completed.stdout + completed.stderr).strip()
            )

    changed = subprocess.run(
        ["git", "-c", "core.quotepath=false", "status", "--short"],
        cwd=root,
        text=True,
        encoding="utf-8",
        capture_output=True,
        check=False,
    )
    if changed.returncode != 0:
        errors.append("git status failed while checking document-only scope")
    else:
        for line in changed.stdout.splitlines():
            path_text = line[3:].strip().strip('"')
            forbidden = (
                path_text == "CMakeLists.txt" or
                (path_text.startswith("src/") and
                 path_text not in ALLOWED_PRODUCTION_REMEDIATIONS) or
                path_text.startswith("include/") or
                (path_text.startswith("tests/") and
                 path_text not in ALLOWED_TEST_REMEDIATIONS)
            )
            if forbidden:
                errors.append(f"unauthorized code scope changed during V6S-00: {path_text}")

    if errors:
        for error in errors:
            print(f"V6S_FREEZE_ERROR {error}")
        return 1
    print(
        "V6S_FREEZE_OK contracts=9 opcodes=94 "
        "candidate_entries=61 remediation_files=5 "
        "readme_selftest=1 manifest_mutation_selftest=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
