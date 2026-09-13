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
SIGNED_CANDIDATE_COMMIT = "0c8e55107d0a9f5c77c744d33e87d466da7f3088"
SIGNED_MANIFEST_SHA256 = (
    "AC97DF798655B1F986D9921A0FC180EBDE71116715F08582D240F26A60C85EC5"
)
SIGNED_ENTRY_COUNT = 61
SIGNED_MIXED_EOL_PATH = "docs/00-项目管理/01-项目操作记录.md"
SIGNED_CRLF_PATH = (
    "docs/10-理论与规划/建议方案/UCN_v6_逻辑模型与伪代码/"
    "13-故障恢复与对抗矩阵.md"
)


def require(text: str, token: str, label: str, errors: list[str]) -> None:
    if token not in text:
        errors.append(f"{label}: missing {token!r}")


def validate_readme_gate(readme: str) -> list[str]:
    """Validate the post-review implementation authorization."""
    errors: list[str] = []
    require(readme, "DONE / EXTERNAL REVIEW GO", "README", errors)
    require(readme, "IMPL-00 = AUTHORIZED / IN PROGRESS", "README", errors)
    require(readme, SIGNED_CANDIDATE_COMMIT, "README", errors)
    require(readme, SIGNED_MANIFEST_SHA256, "README", errors)
    return errors


def selftest_readme_gate() -> list[str]:
    """Prove that the gate rejects a missing external sign-off."""
    errors: list[str] = []
    valid = (
        "DONE / EXTERNAL REVIEW GO\n"
        "IMPL-00 = AUTHORIZED / IN PROGRESS\n"
        f"{SIGNED_CANDIDATE_COMMIT}\n{SIGNED_MANIFEST_SHA256}\n"
    )
    if validate_readme_gate(valid):
        errors.append("README gate self-test rejected the signed GO fixture")
    mutated = valid.replace("DONE / EXTERNAL REVIEW GO", "AUDIT HOLD")
    if not validate_readme_gate(mutated):
        errors.append("README gate self-test accepted authorization without external GO")
    return errors


def validate_live_implementation_status(task: str) -> list[str]:
    """Allow only forward progress after the immutable V6S-00 sign-off."""
    errors: list[str] = []
    match = re.search(r"^IMPL_00\s+=\s+(.+)$", task, re.M)
    if match is None:
        return ["task table: missing IMPL_00 implementation status"]
    status = match.group(1).strip()
    allowed = {
        "AUTHORIZED / IN PROGRESS",
        "DONE / SELF-REVIEW PASS",
        "DONE / SELF-REVIEW PASS / EXTERNAL REVIEW DEFERRED",
    }
    if status not in allowed:
        errors.append(f"task table: invalid IMPL_00 progress state {status!r}")
    return errors


def selftest_live_implementation_status() -> list[str]:
    """Prove completed work does not invalidate the earlier contract sign-off."""
    errors: list[str] = []
    for status in (
        "AUTHORIZED / IN PROGRESS",
        "DONE / SELF-REVIEW PASS",
        "DONE / SELF-REVIEW PASS / EXTERNAL REVIEW DEFERRED",
    ):
        if validate_live_implementation_status(f"IMPL_00 = {status}\n"):
            errors.append(f"implementation status self-test rejected {status!r}")
    if not validate_live_implementation_status("IMPL_00 = BLOCKED\n"):
        errors.append("implementation status self-test accepted a rollback to BLOCKED")
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


def restore_signed_worktree_eol(relative: str, data: bytes) -> bytes | None:
    """Recreate signed Windows bytes after Git's text normalization."""
    if relative == SIGNED_CRLF_PATH:
        return data.replace(b"\n", b"\r\n")
    if relative != SIGNED_MIXED_EOL_PATH:
        return data
    output = bytearray()
    line_break_index = 0
    for value in data:
        if value == 0x0A and (line_break_index == 107 or line_break_index >= 1871):
            output.append(0x0D)
        output.append(value)
        if value == 0x0A:
            line_break_index += 1
    if line_break_index != 5063:
        return None
    return bytes(output)


def git_blob(root: Path, relative: str) -> bytes | None:
    completed = subprocess.run(
        ["git", "show", f"{SIGNED_CANDIDATE_COMMIT}:{relative}"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        return None
    return restore_signed_worktree_eol(relative, completed.stdout)


def validate_signed_manifest(root: Path, manifest_bytes: bytes) -> list[str]:
    """Require the manifest to match every blob in the immutable signed commit."""
    errors: list[str] = []
    if hashlib.sha256(manifest_bytes).hexdigest().upper() != SIGNED_MANIFEST_SHA256:
        errors.append("candidate manifest: signed manifest file hash mismatch")
    try:
        manifest = manifest_bytes.decode("utf-8")
    except UnicodeDecodeError:
        return errors + ["candidate manifest: invalid UTF-8"]
    entries, parse_errors = parse_manifest(manifest)
    errors.extend(parse_errors)
    if len(entries) != SIGNED_ENTRY_COUNT:
        errors.append(
            f"candidate manifest: entries={len(entries)} expected={SIGNED_ENTRY_COUNT}"
        )
    for relative, entry in entries.items():
        data = git_blob(root, relative)
        if data is None:
            errors.append(f"candidate manifest: signed Git blob missing {relative}")
            continue
        if not manifest_entry_matches(entry, data):
            errors.append(f"candidate manifest: signed Git blob mismatch {relative}")
    for relative in REMEDIATION_CONTENT_FILES:
        if relative not in entries:
            errors.append(f"candidate manifest: missing remediation file {relative}")
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", SIGNED_CANDIDATE_COMMIT, "HEAD"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if ancestor.returncode != 0:
        errors.append("current HEAD is not descended from the signed candidate")
    return errors


def selftest_remediation_manifest(root: Path, manifest: str) -> list[str]:
    """Prove every signed remediation blob rejects a same-size one-byte change."""
    entries, parse_errors = parse_manifest(manifest)
    if parse_errors:
        return ["candidate manifest self-test cannot parse valid fixture"]
    errors: list[str] = []
    for relative in REMEDIATION_CONTENT_FILES:
        data = git_blob(root, relative)
        entry = entries.get(relative)
        if not data or not manifest_entry_matches(entry, data):
            errors.append(f"candidate manifest self-test rejected signed bytes {relative}")
            continue
        mutated = bytearray(data)
        mutated[len(mutated) // 2] ^= 1
        if manifest_entry_matches(entry, bytes(mutated)):
            errors.append(f"candidate manifest self-test accepted drift {relative}")
    return errors


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
        elif index < 8 and "DONE / SELF-REVIEW PASS" not in lines[0]:
            errors.append(f"task table: {task_id} is not self-reviewed DONE")
        elif index == 8 and "DONE / EXTERNAL REVIEW GO" not in lines[0]:
            errors.append(f"task table: {task_id} lacks external GO")
    errors.extend(validate_live_implementation_status(task))
    errors.extend(selftest_live_implementation_status())
    require(task, "V6S_00_CONTRACT_FREEZE = DONE / EXTERNAL REVIEW GO",
            "task table", errors)

    readme = texts.get(README_NAME, "")
    errors.extend(validate_readme_gate(readme))
    errors.extend(selftest_readme_gate())

    manifest_path = root / MANIFEST_RELATIVE
    if not manifest_path.is_file():
        errors.append(f"missing candidate manifest: {MANIFEST_RELATIVE}")
    else:
        manifest_bytes = manifest_path.read_bytes()
        manifest = manifest_bytes.decode("utf-8")
        errors.extend(validate_signed_manifest(root, manifest_bytes))
        errors.extend(selftest_remediation_manifest(root, manifest))
        manifest_generator = (
            root / "tools" / "v6" /
            "generate_v6_simplified_docs_manifest.py"
        )
        completed = subprocess.run(
            [sys.executable, str(manifest_generator), "--root", str(root),
             "--output", MANIFEST_RELATIVE, "--check",
             "--treeish", SIGNED_CANDIDATE_COMMIT],
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

    if errors:
        for error in errors:
            print(f"V6S_FREEZE_ERROR {error}")
        return 1
    print(
        "V6S_FREEZE_OK contracts=9 opcodes=94 "
        "candidate_entries=61 remediation_files=5 signed_commit=0c8e551 "
        "readme_selftest=1 manifest_mutation_selftest=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
