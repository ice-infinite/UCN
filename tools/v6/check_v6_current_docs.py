#!/usr/bin/env python3
"""Validate links and current-version markers in the maintained v6 docs."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path
from urllib.parse import unquote


LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
MARKDOWN_TABLE_SEPARATOR = re.compile(r"^:?-{3,}:?$")

PROPOSAL_NAMES = (
    "UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md",
    "UCN_v6_低开销统一Wire重构多方案评估.md",
    "UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md",
    "UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md",
    "UCN_v6_最终协议架构与破坏性重构_RFC.md",
    "UCN_v6_Core_Wire_精确格式_RFC.md",
)

SELF_REVIEWED_PROPOSALS = (
    "UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md",
    "UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md",
    "UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md",
    "UCN_v6_最终协议架构与破坏性重构_RFC.md",
)

SIMPLIFIED_REMEDIATION_FILES = (
    "src/v6/identity/ucn_v6_bootstrap.c",
    "src/v6/runtime/ucn_v6_runtime.c",
    "src/v6/security/ucn_v6_security.c",
    "tests/v6/test_v6_identity.c",
    "tests/v6/test_v6_security.c",
)

MERMAID_CANONICAL_EDGES = frozenset(("-->", "<-->", ".->", "->>", "-->>"))
MERMAID_ANY_EDGE = re.compile(
    r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*([-=.<>()~ox]+)\s*"
    r"(?:\|[^|]*\|\s*)?([A-Za-z][A-Za-z0-9_]*)",
)
MERMAID_PARTICIPANT = re.compile(
    r"^\s*(?:participant|actor)\s+([A-Za-z][A-Za-z0-9_]*)"
    r"(?:\s+as\s+(.+?))?\s*$"
)
MERMAID_NODE = re.compile(
    r"\b([A-Za-z][A-Za-z0-9_]*)\s*"
    r"(?:\[([^\]]+)\]|\(([^)]+)\)|\{([^}]+)\})"
)
MERMAID_EDGE_GLYPH = re.compile(
    r"(?:--+|==+|~~+|-\.|\.-|<[-=.]+|[-=.]+>|-\)|--[ox]|[ox]--)",
    re.IGNORECASE,
)

OWNER_ID_PATTERNS = (
    ("OWNER_COORDINATOR", re.compile(
        r"\b(?:runtime\s+setup\s+coordinator|runtime\s+coordinator|coordinator|"
        r"runtime\s+owner|protocol\s+owner)\b", re.IGNORECASE)),
    ("OWNER_PERSISTENCE", re.compile(r"\bpersistence\s*owner\b", re.IGNORECASE)),
    ("OWNER_ADAPTER", re.compile(r"\badapter\s*owner\b", re.IGNORECASE)),
    ("OWNER_CLUSTER", re.compile(r"\bcluster\s*owner\b", re.IGNORECASE)),
    ("OWNER_GROUP", re.compile(r"\bgroup\s*owner\b", re.IGNORECASE)),
    ("OWNER_TIME", re.compile(
        r"\b(?:(?:time\s+domain|time|realtime)\s*owner)\b", re.IGNORECASE)),
    ("OWNER_OPERATION", re.compile(
        r"\b(?:(?:operation|service)\s*owner)\b", re.IGNORECASE)),
    ("OWNER_TRANSPORT", re.compile(
        r"\b(?:(?:transport|transfer)\s*owner)\b", re.IGNORECASE)),
    ("OWNER_FLOW", re.compile(r"\bflow\s*owner\b", re.IGNORECASE)),
    ("OWNER_ROUTE", re.compile(
        r"\b(?:(?:advanced\s*)?rout(?:e|ing)\s*owner)\b", re.IGNORECASE)),
    ("OWNER_CAPABILITY", re.compile(r"\bcapabilit(?:y|ies)\s*owner(?:s)?\b", re.IGNORECASE)),
    ("OWNER_SECURITY", re.compile(
        r"\b(?:(?:peer\s+session|security)\s*owner)\b", re.IGNORECASE)),
    ("OWNER_IDENTITY", re.compile(r"\bidentity\s*owner\b", re.IGNORECASE)),
    ("OWNER_ADMISSION", re.compile(r"\badmission\s*owner\b", re.IGNORECASE)),
    ("OWNER_REQUEST", re.compile(r"\brequest(?:ing)?\s*owner\b", re.IGNORECASE)),
    ("OWNER_SEQUENCE", re.compile(r"\bsequence\s*owner\b", re.IGNORECASE)),
    ("OWNER_QOS", re.compile(r"\b(?:queue|qos)\s*owner\b", re.IGNORECASE)),
    ("OWNER_MODULE", re.compile(
        r"\b(?:exact\s+)?(?:module|caller|business|send(?:ing)?|durable)\s*owner(?:s)?\b",
        re.IGNORECASE)),
)


def _humanize_mermaid_identifier(identifier: str) -> str:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", identifier)
    return value.replace("_", " ")


def _canonical_owner_id(label_or_identifier: str) -> str | None:
    raw = re.sub(r"<[^>]+>", " ", label_or_identifier).strip()
    canonical_ids = {owner_id for pair in EXPECTED_OWNER_CONNECTIONS for owner_id in pair}
    if raw in canonical_ids:
        return raw
    for owner_id, pattern in OWNER_ID_PATTERNS:
        if pattern.search(raw) or pattern.search(_humanize_mermaid_identifier(raw)):
            return owner_id
    return None


def _looks_like_owner_endpoint(label_or_identifier: str) -> bool:
    raw = re.sub(r"<[^>]+>", " ", label_or_identifier).strip()
    if _canonical_owner_id(raw) is not None:
        return True
    return re.search(r"\bowner(?:s)?\s*$", _humanize_mermaid_identifier(raw), re.IGNORECASE) is not None


def _mermaid_owner_edge_findings(text: str) -> list[tuple[int, str]]:
    """Reject unparseable Owner edges and edges outside the canonical registry."""
    findings: list[tuple[int, str]] = []
    lines = text.splitlines()
    in_mermaid = False
    block_start = 0
    block_lines: list[str] = []

    def inspect_block(start: int, block: list[str]) -> None:
        aliases: dict[str, str] = {}
        for raw in block:
            participant = MERMAID_PARTICIPANT.match(raw)
            if participant:
                aliases[participant.group(1)] = (
                    participant.group(2).strip()
                    if participant.group(2) is not None
                    else _humanize_mermaid_identifier(participant.group(1))
                )
            for node in MERMAID_NODE.finditer(raw):
                label = next(value for value in node.groups()[1:] if value is not None)
                aliases[node.group(1)] = label.strip()
        owner_aliases = {
            alias for alias, label in aliases.items()
            if _canonical_owner_id(label) is not None
        }
        for offset, raw in enumerate(block, start=1):
            normalized = MERMAID_NODE.sub(lambda match: match.group(1), raw)
            referenced_owner = any(
                re.search(rf"\b{re.escape(alias)}\b", normalized)
                for alias in owner_aliases
            )
            if not referenced_owner:
                referenced_owner = any(
                    _canonical_owner_id(identifier) is not None
                    for identifier in re.findall(r"\b[A-Za-z][A-Za-z0-9_]*\b", normalized)
                )
            edge = MERMAID_ANY_EDGE.match(normalized)
            if not edge:
                if referenced_owner and MERMAID_EDGE_GLYPH.search(normalized):
                    findings.append((start + offset, raw.strip()))
                continue
            source_id, operator, target_id = edge.groups()
            source = aliases.get(source_id, source_id)
            target = aliases.get(target_id, target_id)
            source_owner = _canonical_owner_id(source)
            target_owner = _canonical_owner_id(target)
            has_owner = referenced_owner or source_owner is not None or target_owner is not None
            remainder_before_message = normalized[edge.end():].split(":", 1)[0]
            if has_owner and MERMAID_EDGE_GLYPH.search(remainder_before_message):
                findings.append((start + offset, raw.strip()))
                continue
            if has_owner and operator not in MERMAID_CANONICAL_EDGES:
                findings.append((start + offset, raw.strip()))
                continue
            if not has_owner:
                continue
            if (_looks_like_owner_endpoint(source) and source_owner is None) or (
                    _looks_like_owner_endpoint(target) and target_owner is None):
                findings.append((start + offset, raw.strip()))
                continue
            if source_owner is None or target_owner is None:
                continue
            if source_id == target_id and source_owner == target_owner:
                continue
            directed_pairs = (
                ((source_owner, target_owner), (target_owner, source_owner))
                if operator == "<-->"
                else ((source_owner, target_owner),)
            )
            if any(pair not in EXPECTED_OWNER_CONNECTIONS for pair in directed_pairs):
                findings.append((start + offset, raw.strip()))

    for line_number, line in enumerate(lines, start=1):
        if not in_mermaid and line.strip().lower() == "```mermaid":
            in_mermaid = True
            block_start = line_number
            block_lines = []
            continue
        if in_mermaid and line.strip() == "```":
            inspect_block(block_start, block_lines)
            in_mermaid = False
            block_lines = []
            continue
        if in_mermaid:
            block_lines.append(line)
    return findings


def _pseudocode_owner_bypass_findings(
        text: str, allowed_infrastructure: frozenset[str]) -> list[tuple[int, str]]:
    """Reject non-Coordinator infrastructure calls and simple alias propagation."""
    findings: list[tuple[int, str]] = []
    in_fence = False
    language = ""
    block_start = 0
    block_lines: list[str] = []
    forbidden = tuple(sorted({"persistence", "adapter"} - set(allowed_infrastructure)))

    def inspect_block(start: int, block: list[str]) -> None:
        block_text = "\n".join(block)
        matches: list[tuple[int, int, str]] = []
        bypass_patterns: list[re.Pattern[str]] = [
            re.compile(
                r"\b(?:admission|security|transport|operation|group|cluster|identity|"
                r"route|routing|flow|transfer|realtime|time|service|business|caller)"
                r"(?:[_\s]*owner)?\s*(?:->|\.)\s*(?:persistence|adapter)"
                r"(?:[_\s]*owner)?\s*(?:->|\.)",
                re.IGNORECASE,
            ),
        ]
        for infrastructure in forbidden:
            bypass_patterns.extend((
                re.compile(
                    rf"\b{infrastructure}(?:[_\s]*owner)?\s*(?:\.|->)\s*"
                    r"[A-Za-z_][A-Za-z0-9_]*\s*\(",
                    re.IGNORECASE,
                ),
                re.compile(
                    rf"\b{infrastructure}_[A-Za-z0-9_]*\s*\(",
                    re.IGNORECASE,
                ),
                re.compile(
                    rf"^\s*(?:call|request|invoke)\s+(?:the\s+)?{infrastructure}"
                    r"(?:\s+Owner)?\b",
                    re.IGNORECASE | re.MULTILINE,
                ),
            ))
        if "persistence" in forbidden:
            bypass_patterns.append(
                re.compile(r"^\s*persist\s+", re.IGNORECASE | re.MULTILINE)
            )

        for pattern in bypass_patterns:
            for match in pattern.finditer(block_text):
                line_number = start + block_text[:match.start()].count("\n") + 1
                matches.append((match.start(), line_number, match.group(0).strip()))

        call_expression = re.compile(
            r"\b([A-Za-z_][A-Za-z0-9_]*(?:\s*(?:\.|->)\s*"
            r"[A-Za-z_][A-Za-z0-9_]*)*)\s*\(",
            re.IGNORECASE,
        )
        for match in call_expression.finditer(block_text):
            callee = re.sub(r"\s+", "", match.group(1)).casefold()
            allowed_route = (
                callee.startswith("coordinator_route_persistence_") or
                callee.startswith("coordinator_route_adapter_")
            )
            if allowed_route:
                continue
            words = re.split(r"[_\.>\-]+", callee)
            kind = None
            if any("persistence" in word for word in words):
                kind = "persistence"
            elif any("adapter" in word for word in words):
                kind = "adapter"
            if kind is not None and kind in forbidden:
                line_number = start + block_text[:match.start()].count("\n") + 1
                matches.append((match.start(), line_number, match.group(0).strip()))

        alias_kind: dict[str, str] = {}
        assignment = re.compile(
            r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
            r"([^;\n]+?)\s*(?:;|$)",
            re.IGNORECASE | re.MULTILINE,
        )
        changed = True
        while changed:
            changed = False
            for assign in assignment.finditer(block_text):
                alias, source_expression = assign.groups()
                source_key = source_expression.strip().casefold()
                kind = alias_kind.get(source_key)
                if kind is None:
                    for candidate in forbidden:
                        marker = re.compile(
                            rf"\b{candidate}(?:[_\s]*owner)?\b", re.IGNORECASE
                        )
                        if marker.search(source_expression):
                            kind = candidate
                            break
                if kind is None:
                    for source_alias, source_kind in alias_kind.items():
                        if re.search(rf"\b{re.escape(source_alias)}\b", source_expression,
                                     re.IGNORECASE):
                            kind = source_kind
                            break
                if kind is not None and alias.casefold() not in alias_kind:
                    alias_kind[alias.casefold()] = kind
                    changed = True

        for alias, kind in alias_kind.items():
            if kind not in forbidden:
                continue
            alias_call = re.compile(
                rf"\b{re.escape(alias)}\s*(?:\.|->)\s*"
                r"[A-Za-z_][A-Za-z0-9_]*\s*\(",
                re.IGNORECASE,
            )
            for match in alias_call.finditer(block_text):
                line_number = start + block_text[:match.start()].count("\n") + 1
                matches.append((match.start(), line_number, match.group(0).strip()))

        seen: set[tuple[int, str]] = set()
        for _, line_number, statement in sorted(matches):
            key = (line_number, statement.casefold())
            if key not in seen:
                findings.append((line_number, statement))
                seen.add(key)

    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("```"):
            if not in_fence:
                in_fence = True
                language = stripped[3:].strip().lower()
                block_start = line_number
                block_lines = []
            else:
                if language != "mermaid":
                    inspect_block(block_start, block_lines)
                in_fence = False
                language = ""
                block_lines = []
            continue
        if in_fence:
            block_lines.append(line)
    return findings


def owner_boundary_findings(
        text: str,
        allowed_infrastructure: frozenset[str] = frozenset()) -> list[tuple[int, str]]:
    return _mermaid_owner_edge_findings(text) + _pseudocode_owner_bypass_findings(
        text, allowed_infrastructure
    )


def _split_markdown_table_row(line: str) -> list[str] | None:
    """Split one conventional pipe table row without counting escaped/code pipes."""
    stripped = line.strip()
    if not stripped.startswith("|") or not stripped.endswith("|"):
        return None
    body = stripped[1:-1]
    cells: list[str] = []
    current: list[str] = []
    code_fence = 0
    index = 0
    while index < len(body):
        character = body[index]
        if character == "\\" and index + 1 < len(body):
            current.extend((character, body[index + 1]))
            index += 2
            continue
        if character == "`":
            run = 1
            while index + run < len(body) and body[index + run] == "`":
                run += 1
            if code_fence == 0:
                code_fence = run
            elif code_fence == run:
                code_fence = 0
            current.extend("`" * run)
            index += run
            continue
        if character == "|" and code_fence == 0:
            cells.append("".join(current).strip())
            current = []
        else:
            current.append(character)
        index += 1
    cells.append("".join(current).strip())
    return cells


def markdown_table_findings(text: str) -> list[tuple[int, str]]:
    """Return malformed maintained Markdown tables as line-scoped findings."""
    findings: list[tuple[int, str]] = []
    lines = text.splitlines()
    in_fence = False
    index = 0
    while index < len(lines):
        stripped = lines[index].strip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            index += 1
            continue
        row = _split_markdown_table_row(lines[index]) if not in_fence else None
        if row is None:
            index += 1
            continue

        start = index
        block: list[tuple[int, list[str], str]] = []
        while index < len(lines):
            candidate = _split_markdown_table_row(lines[index])
            if candidate is None:
                break
            block.append((index + 1, candidate, lines[index].strip()))
            index += 1

        if len(block) < 2:
            continue
        separator_cells = block[1][1]
        if not separator_cells or not all(
                MARKDOWN_TABLE_SEPARATOR.fullmatch(cell) for cell in separator_cells):
            continue
        expected = len(block[0][1])
        if len(separator_cells) != expected:
            findings.append((block[1][0], block[1][2]))
        for line_number, cells, raw in block[2:]:
            if len(cells) != expected:
                findings.append((line_number, raw))
    return findings


def validate_markdown_table_scanner_selftest(errors: list[str]) -> None:
    """Prove the table gate rejects shape drift without rejecting literal pipes."""
    good = """| A | B |
| --- | ---: |
| escaped \\| pipe | `x | y` |
"""
    if markdown_table_findings(good):
        errors.append("markdown-table scanner rejected escaped/code-pipe fixture")

    bad_fixtures = (
        """| A | B |
| --- | --- |
| only one |
""",
        """| A | B |
| --- | --- |
| one | two | three |
""",
        """| A | B |
| --- |
| one | two |
""",
    )
    for fixture_index, fixture in enumerate(bad_fixtures, start=1):
        if not markdown_table_findings(fixture):
            errors.append(
                f"markdown-table scanner selftest missed bad fixture {fixture_index}"
            )


def validate_owner_boundary_scanner_selftest(errors: list[str]) -> None:
    """Mutation-style fixtures ensure the R15/R22 gate detects real bypasses."""
    bad_fixtures = (
        """```mermaid
flowchart LR
S[Security Owner] --> P[Persistence Owner]
```""",
        """```mermaid
sequenceDiagram
participant C as Caller Owner
participant P as Persistence Owner
C->>P: submit record
```""",
        """```mermaid
sequenceDiagram
participant C as Caller Owner
participant P as Persistence Owner
P-->>C: exact reload proof
```""",
        """```mermaid
flowchart LR
C[Cluster Owner] --> A[Adapter Owner]
```""",
        """```mermaid
flowchart LR
S[Security Owner] --> C[Cluster Owner]
```""",
        """```mermaid
flowchart LR
S[Security Owner] ==> P[Persistence Owner]
```""",
        """```mermaid
flowchart LR
P[Persistence Owner] <-- O[Operation Owner]
```""",
        """```mermaid
sequenceDiagram
participant P as Persistence Owner
participant O as Operation Owner
P-)O: direct completion
```""",
        """```mermaid
flowchart LR
S[Security Owner] -- hidden --> P[Persistence Owner]
```""",
        """```mermaid
flowchart LR
R[Runtime Coordinator] --> P[Persistence Owner] --> O[Operation Owner]
```""",
        """```mermaid
flowchart LR
S[Security Owner] ~~~ P[Persistence Owner]
```""",
        """```mermaid
flowchart LR
RT[Realtime] -. 不依赖 .- CL[Cluster Owner]
```""",
        """```mermaid
sequenceDiagram
participant SecurityOwner
participant PersistenceOwner
SecurityOwner->>PersistenceOwner: submit
```""",
        """```mermaid
flowchart LR
R[Runtime Coordinator] --> X[Unknown Owner]
```""",
        """```text
security_owner->persistence_owner.submit(requirement)
```""",
        """```text
persistence_submit(requirement)
```""",
        """```text
security_owner
    .persistence_owner
    .submit(requirement)
```""",
        """```text
persist CONFIG_COMMIT
```""",
        """```text
persistence.submit(requirement)
```""",
        """```text
PersistenceOwner.submit(requirement)
```""",
        """```text
store = persistence_owner;
store.submit(requirement)
```""",
        """```text
adapter.submit(frame)
```""",
        """```text
my_adapter.submit(frame)
```""",
        """```text
owner = getPersistenceOwner()
```""",
    )
    for index, fixture in enumerate(bad_fixtures, start=1):
        if not owner_boundary_findings(fixture):
            errors.append(f"owner-boundary scanner selftest missed bad fixture {index}")

    good_fixture = """```mermaid
sequenceDiagram
participant C as Caller Owner
participant R as Runtime Coordinator
participant P as Persistence Owner
participant V as Storage Provider
C->>R: immutable requirement
R->>P: exact requirement
P->>V: submit
V-->>P: completion
P-->>R: exact proof event
R-->>C: routed proof event
```"""
    if owner_boundary_findings(good_fixture):
        errors.append("owner-boundary scanner selftest rejected Coordinator-mediated fixture")

    cluster_fixture = """```mermaid
flowchart LR
R[Runtime Coordinator] --> C[Cluster Owner]
C --> R
```"""
    if owner_boundary_findings(cluster_fixture):
        errors.append("owner-boundary scanner selftest rejected registered Cluster edges")


OWNER_CONNECTION_REGISTRY_BEGIN = "<!-- V6_OWNER_CONNECTION_REGISTRY_BEGIN -->"
OWNER_CONNECTION_REGISTRY_END = "<!-- V6_OWNER_CONNECTION_REGISTRY_END -->"
EXPECTED_OWNER_CONNECTIONS = (
    ("OWNER_COORDINATOR", "OWNER_IDENTITY"),
    ("OWNER_IDENTITY", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_SECURITY"),
    ("OWNER_SECURITY", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_CAPABILITY"),
    ("OWNER_CAPABILITY", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_ROUTE"),
    ("OWNER_ROUTE", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_FLOW"),
    ("OWNER_FLOW", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_TRANSPORT"),
    ("OWNER_TRANSPORT", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_OPERATION"),
    ("OWNER_OPERATION", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_TIME"),
    ("OWNER_TIME", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_GROUP"),
    ("OWNER_GROUP", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_CLUSTER"),
    ("OWNER_CLUSTER", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_PERSISTENCE"),
    ("OWNER_PERSISTENCE", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_ADAPTER"),
    ("OWNER_ADAPTER", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_ADMISSION"),
    ("OWNER_ADMISSION", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_REQUEST"),
    ("OWNER_REQUEST", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_SEQUENCE"),
    ("OWNER_SEQUENCE", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_QOS"),
    ("OWNER_QOS", "OWNER_COORDINATOR"),
    ("OWNER_COORDINATOR", "OWNER_MODULE"),
    ("OWNER_MODULE", "OWNER_COORDINATOR"),
)


def validate_owner_connection_registry(
        errors: list[str], path: Path, text: str) -> None:
    """Validate the sole machine-readable cross-Owner connection registry."""
    if text.count(OWNER_CONNECTION_REGISTRY_BEGIN) != 1:
        errors.append(f"owner connection registry begin marker count invalid: {path}")
        return
    if text.count(OWNER_CONNECTION_REGISTRY_END) != 1:
        errors.append(f"owner connection registry end marker count invalid: {path}")
        return
    start = text.index(OWNER_CONNECTION_REGISTRY_BEGIN) + len(
        OWNER_CONNECTION_REGISTRY_BEGIN
    )
    end = text.index(OWNER_CONNECTION_REGISTRY_END, start)
    rows: list[tuple[str, str]] = []
    for line in text[start:end].splitlines():
        if not line.lstrip().startswith("|"):
            continue
        cells = [cell.strip().strip("`") for cell in line.strip().strip("|").split("|")]
        if (len(cells) < 2 or cells[0] in {"调用者", "Source Owner ID"} or
                set(cells[0]) <= {"-", ":"}):
            continue
        rows.append((cells[0], cells[1]))
    if tuple(rows) != EXPECTED_OWNER_CONNECTIONS:
        errors.append(f"owner connection registry mismatch: {path} -> {rows}")


def validate_owner_connection_registry_selftest(errors: list[str]) -> None:
    """Prove the registry parser accepts the canonical table and rejects mutation."""
    header = "| 调用者 | 目标 Owner |\n| --- | --- |\n"
    body = "".join(f"| {caller} | {target} |\n" for caller, target in EXPECTED_OWNER_CONNECTIONS)
    canonical = (
        OWNER_CONNECTION_REGISTRY_BEGIN + "\n" + header + body +
        OWNER_CONNECTION_REGISTRY_END
    )
    local_errors: list[str] = []
    fixture_path = Path("<owner-connection-registry-selftest>")
    validate_owner_connection_registry(local_errors, fixture_path, canonical)
    if local_errors:
        errors.append("owner connection registry selftest rejected canonical fixture")

    mutated = canonical.replace(
        "| OWNER_COORDINATOR | OWNER_SECURITY |",
        "| OWNER_SECURITY | OWNER_PERSISTENCE |",
        1,
    )
    local_errors = []
    validate_owner_connection_registry(local_errors, fixture_path, mutated)
    if not local_errors:
        errors.append("owner connection registry selftest missed direct-Owner mutation")


def active_documents(root: Path) -> list[Path]:
    docs = [root / "README.md", root / "docs" / "README.md"]
    for name in ("official", "用户手册", "源码阅读指南", "reference", "evidence", "experimental", "calltree"):
        docs.extend(sorted((root / "docs" / name).rglob("*.md")))
    management = root / "docs" / "00-项目管理"
    for name in (
        "00-任务表.md",
        "01-项目操作记录.md",
        "05-V6兼容删除清单.md",
        "06-V6简化版分支基线与实施入口.md",
    ):
        docs.append(management / name)
    docs.extend(sorted((root / "docs" / "08-实现与验证" / "版本演进").glob("UCN_V6_*.md")))
    proposal = root / "docs" / "10-理论与规划" / "建议方案"
    docs.extend(proposal / name for name in PROPOSAL_NAMES)
    docs.extend(sorted((proposal / "UCN_v6_逻辑模型与伪代码").glob("*.md")))
    docs.extend(sorted((proposal / "UCN_v6_V6S_00_简化版实施合同冻结").glob("*.md")))
    docs.append(
        root / "docs" / "09-审计与整改" /
        "UCN_V6_简化文档收口与全体自审报告_2026-09-08.md"
    )
    return sorted(set(path.resolve() for path in docs))


def require_contains(errors: list[str], path: Path, needle: str, label: str) -> None:
    if not path.is_file():
        return
    if needle not in path.read_text(encoding="utf-8"):
        errors.append(f"missing {label}: {path}")


def validate_simplified_contract(root: Path, errors: list[str]) -> None:
    proposal = root / "docs" / "10-理论与规划" / "建议方案"
    logic = proposal / "UCN_v6_逻辑模型与伪代码"
    numbered: dict[int, list[Path]] = {number: [] for number in range(27)}
    for path in logic.glob("*.md"):
        match = re.match(r"^(\d\d)-", path.name)
        if match:
            number = int(match.group(1))
            if number in numbered:
                numbered[number].append(path)
    for number, matches in numbered.items():
        if len(matches) != 1:
            errors.append(
                f"logic document number {number:02d} count={len(matches)} expected=1"
            )

    for number in range(27):
        matches = numbered[number]
        if len(matches) != 1:
            continue
        text = matches[0].read_text(encoding="utf-8")
        prefix = "\n".join(text.splitlines()[:10])
        if "SELF-REVIEWED / EXTERNAL REVIEW REQUIRED" not in prefix:
            errors.append(f"logic document lacks review fence: {matches[0]}")
        if "```mermaid" not in text:
            errors.append(f"logic document lacks architecture/state diagram: {matches[0]}")
        headings = [
            int(match.group(1))
            for line in text.splitlines()
            if (match := re.match(r"^## (\d+)\. ", line))
        ]
        if headings != list(range(1, len(headings) + 1)):
            errors.append(
                f"non-sequential logic headings: {matches[0]} -> {headings}"
            )

        allowed_infrastructure = (
            frozenset(("adapter",)) if number == 5 else
            frozenset(("persistence",)) if number in (12, 22) else
            frozenset()
        )
        for line_number, statement in owner_boundary_findings(
                text, allowed_infrastructure=allowed_infrastructure):
            errors.append(
                f"owner boundary violation: "
                f"{matches[0]}:{line_number}: {statement}"
            )

    validate_owner_boundary_scanner_selftest(errors)
    validate_owner_connection_registry_selftest(errors)
    validate_markdown_table_scanner_selftest(errors)

    for name in SELF_REVIEWED_PROPOSALS:
        path = proposal / name
        if not path.is_file():
            continue
        prefix = "\n".join(path.read_text(encoding="utf-8").splitlines()[:12])
        if "SELF-REVIEWED" not in prefix or "EXTERNAL" not in prefix:
            errors.append(f"proposal lacks current self-review fence: {path}")
        if "PROPOSED / REVIEW REQUIRED" in prefix:
            errors.append(f"stale proposal status: {path}")

    readme = logic / "README.md"
    require_contains(errors, readme, "`00～14`", "full-contract document range")
    require_contains(errors, readme, "`15～26`", "simplified document range")

    route_contracts = (
        logic / "07-Route-Discovery-Forwarder.md",
        logic / "15-基础通信与自动路由简化设计.md",
        logic / "24-Advanced-Route与Flow简化设计.md",
        proposal / "UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md",
        proposal / "UCN_v6_最终协议架构与破坏性重构_RFC.md",
    )
    for path in route_contracts:
        require_contains(errors, path, "SoftRoute", "SoftRoute contract")
        require_contains(errors, path, "FlowPath", "FlowPath contract")

    for path in (
        logic / "07-Route-Discovery-Forwarder.md",
        logic / "24-Advanced-Route与Flow简化设计.md",
    ):
        require_contains(errors, path, "origin_principal", "RouteDomain origin principal")
        require_contains(errors, path, "destination_principal", "RouteDomain destination principal")
        if path.is_file() and not re.search(
                r"realm,?\s+origin_principal,?\s+origin_address,?\s+"
                r"origin_binding_generation,?\s+origin_session_generation,?\s+"
                r"destination_principal,?\s+destination_address,?\s+"
                r"destination_binding_generation",
                path.read_text(encoding="utf-8")):
            errors.append(f"non-canonical RouteDomain field order: {path}")

    basic = logic / "15-基础通信与自动路由简化设计.md"
    if basic.is_file() and "RREQ → RREP → ACTIVE" in basic.read_text(encoding="utf-8"):
        errors.append(f"basic route still grants ACTIVE directly: {basic}")

    advanced = logic / "24-Advanced-Route与Flow简化设计.md"
    for opcode in (
        "PATH_ACTIVATE_STAGE",
        "PATH_STAGE_ACK",
        "PATH_ACTIVATE_COMMIT",
        "PATH_COMMIT_ACK",
        "PATH_ACTIVATE_ABORT",
    ):
        require_contains(errors, advanced, opcode, f"advanced Flow opcode {opcode}")

    api = logic / "25-统一公共API与内部SPI冻结候选.md"
    for symbol in (
        "ucn_publish(",
        "ucn_request(",
        "ucn_send_query(",
        "ucn_driver_rx_publish(",
        "module_ensure(",
        "owner_instance",
        "object_kind",
        "REMOTE_INBOX_ACCEPTED",
    ):
        require_contains(errors, api, symbol, f"unified API/SPI symbol {symbol}")

    invariant_registry = logic / "26-全局不变量与模块对接登记表.md"
    for invariant in (
        "INV-SEC-SESSION-PERSIST",
        "INV-PERSIST-COMMIT-ORDER",
        "INV-PERSIST-DOMAIN-ISOLATION",
        "INV-CLUSTER-DURABLE-BEFORE-PROMISE",
        "INV-REALTIME-DOMAIN-PERSIST",
        "INV-DRIVER-GATE-ORDER",
        "INV-OWNER-STEP-FAIRNESS",
        "INV-RESOLVER-TYPED-DEPENDENCY",
        "INV-HOP-BUDGET-DROP",
        "INV-RERR-CAUSAL",
        "INV-FEATURE-PHYSICAL-ISOLATION",
        "INV-PERSIST-MIGRATION",
    ):
        require_contains(errors, invariant_registry, invariant, f"global invariant {invariant}")
    for needle in (
        "DependencyRequirement",
        "DependencyHandle",
        "module_ensure",
        "Feature OFF",
        "旧 Store",
        "digest 都**只作查找预筛选**",
        "canonical requirement 不同属于冲突输入",
        "尚未经过密码认证的 Driver/Link Facts",
    ):
        require_contains(errors, invariant_registry, needle, f"module registry contract {needle}")
    if invariant_registry.is_file():
        registry_text = invariant_registry.read_text(encoding="utf-8")
        validate_owner_connection_registry(errors, invariant_registry, registry_text)
        if "authenticated Driver facts" in registry_text:
            errors.append(f"raw Driver facts are still described as authenticated: {invariant_registry}")

    for path in (
        proposal / "UCN_v6_最终协议架构与破坏性重构_RFC.md",
        proposal / "UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md",
        proposal / "UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md",
        logic / "README.md",
        logic / "01-总体执行模型与模块协作.md",
        logic / "02-最小发送与接收闭环.md",
        logic / "03-Request-Attempt-Buffer生命周期.md",
        logic / "15-基础通信与自动路由简化设计.md",
        logic / "25-统一公共API与内部SPI冻结候选.md",
    ):
        require_contains(errors, path, "未跟踪", "untracked minimal publish contract")
        require_contains(errors, path, "TX Slot", "single TX-slot fast path")
        if path.is_file() and not re.search(
                r"operation_id\s*={1,2}\s*0",
                path.read_text(encoding="utf-8")):
            errors.append(f"untracked path lacks zero operation-id rule: {path}")

    service = logic / "18-Service请求响应与QoS简化设计.md"
    for needle in (
        "EXECUTING durable + reload before handoff",
        "ABORTED_NO_EFFECT",
        "REMOTE_INBOX_ACCEPTED",
        "REMOTE_REASSEMBLED",
    ):
        require_contains(errors, service, needle, f"Service durable/completion contract {needle}")

    persistence_simple = logic / "22-持久化与掉电恢复简化设计.md"
    require_contains(
        errors,
        persistence_simple,
        "ABORTED_NO_EFFECT",
        "simplified Persistence durable no-effect terminal",
    )
    for needle in (
        "PersistDomain",
        "write uncommitted inactive slot",
        "atomically publish the slot commit marker",
        "checked-advance anti-rollback witness",
        "reload committed slot",
    ):
        require_contains(errors, persistence_simple, needle, f"Persistence ordering/domain {needle}")
    require_contains(
        errors,
        persistence_simple,
        "Persistence Owner → Coordinator → requesting Owner",
        "Coordinator-routed persistence proof path",
    )
    if persistence_simple.is_file():
        persistence_text = persistence_simple.read_text(encoding="utf-8")
        ordered = [
            persistence_text.find("write uncommitted inactive slot"),
            persistence_text.find("atomically publish the slot commit marker"),
            persistence_text.find("checked-advance anti-rollback witness"),
            persistence_text.find("reload committed slot"),
        ]
        if min(ordered) < 0 or ordered != sorted(ordered):
            errors.append(f"non-canonical Persistence commit order: {persistence_simple}")

    security_simple = logic / "16-身份准入与安全简化设计.md"
    for needle in ("NEED_PERSISTENCE", "security_on_durability_proof", "reload"):
        require_contains(errors, security_simple, needle, f"Security durable session gate {needle}")

    cluster_simple = logic / "21-Cluster簇管理简化设计.md"
    for needle in ("complete vote evidence", "exact reload", "TAKEOVER_ACK"):
        require_contains(errors, cluster_simple, needle, f"Cluster durability proof {needle}")

    for needle in (
        "S-->>R: exact reload view events",
        "R-->>O: route exact reload view events",
    ):
        require_contains(errors, invariant_registry, needle, f"Coordinator-routed init proof {needle}")

    realtime_simple = logic / "19-实时通信与时间同步简化设计.md"
    for needle in (
        "GENERATION_LOADING",
        "time_domain_on_durable_proof",
        "DROP_EXPIRED",
    ):
        require_contains(errors, realtime_simple, needle, f"Realtime generation/budget gate {needle}")
    if realtime_simple.is_file() and "按显式策略降级" in realtime_simple.read_text(encoding="utf-8"):
        errors.append(f"Realtime Hop Budget still permits policy downgrade: {realtime_simple}")

    for needle in (
        "call_driver",
        "StepBudgetPlan",
        "original traffic RouteDomain",
        "route_generation",
        "route_causal_id",
        "failed link generation",
    ):
        require_contains(errors, basic, needle, f"basic driver/fairness/RERR gate {needle}")

    if api.is_file():
        api_text = api.read_text(encoding="utf-8")
        if "fixed priority order" in api_text:
            errors.append(f"strict fixed-priority owner step remains: {api}")
        for needle in (
            "StepBudgetPlan + per-class cap + persistent rotating cursor",
            "digest 只作查找预筛选",
            "canonical requirement",
        ):
            if needle not in api_text:
                errors.append(f"missing API/SPI fairness or exact dependency rule {needle}: {api}")

    resolver_documents = (
        logic / "01-总体执行模型与模块协作.md",
        logic / "23-Capability与Contract-Resolver简化设计.md",
        logic / "25-统一公共API与内部SPI冻结候选.md",
    )
    stale_resolver = re.compile(
        r"\bNEED_(?:ROUTE|SECURITY(?:_CONTEXT)?|FLOW(?:_CONTEXT)?|TRANSFER|TIME)\b"
    )
    for path in resolver_documents:
        if path.is_file() and stale_resolver.search(path.read_text(encoding="utf-8")):
            errors.append(f"stale untyped Resolver result: {path}")

    for path in logic.glob("*.md"):
        if path.is_file() and "fixed priority order" in path.read_text(encoding="utf-8"):
            errors.append(f"strict fixed-priority scheduling remains: {path}")

    minimum = logic / "02-最小发送与接收闭环.md"
    require_contains(errors, minimum, "本地发送所有权根", "single local send ownership root")
    require_contains(errors, minimum, "ucn_publish(", "minimum-path public publish API")
    if minimum.is_file():
        minimum_text = minimum.read_text(encoding="utf-8")
        if "一个 API 调用只创建一个逻辑 Send Request" in minimum_text:
            errors.append(f"minimum path still requires a Request for every call: {minimum}")
        if "App->>Runtime: send(" in minimum_text:
            errors.append(f"minimum path retains stale public send API name: {minimum}")

    basic_simple = logic / "15-基础通信与自动路由简化设计.md"
    require_contains(errors, basic_simple, "ucn_driver_rx_publish(", "basic Driver fact API")
    if basic_simple.is_file() and re.search(
            r"(?m)^driver_rx\(", basic_simple.read_text(encoding="utf-8")):
        errors.append(f"basic path retains stale Driver fact API name: {basic_simple}")

    module_design = proposal / "UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md"
    if module_design.is_file() and "所有路径共享唯一 Request Finalizer" in module_design.read_text(encoding="utf-8"):
        errors.append(f"untracked path is incorrectly assigned a Request Finalizer: {module_design}")
    for needle in (
        "UCN_FEATURE_REALTIME",
        "UCN_FEATURE_CLUSTER",
        "UCN_FEATURE_ADAPTER",
        "PUBLIC",
        "Adapter OFF + Runtime ON",
        "多个隔离 durable domain",
        "新旧实现不得生产双写",
        "旧 Store vtable",
        "rg/nm/CMake",
    ):
        require_contains(errors, module_design, needle, f"physical modularization/migration gap {needle}")
    if module_design.is_file():
        module_text = module_design.read_text(encoding="utf-8")
        for stale_option in (
            "UCN_V6_ENABLE_REALTIME",
            "UCN_V6_ENABLE_CLUSTER",
            "UCN_V6_ENABLE_ADAPTER",
        ):
            if stale_option in module_text:
                errors.append(f"stale CMake option {stale_option}: {module_design}")

    service_simple = logic / "18-Service请求响应与QoS简化设计.md"
    if service_simple.is_file() and "QueueItem" in service_simple.read_text(encoding="utf-8"):
        errors.append(f"Service simplification retains a second QueueItem owner: {service_simple}")

    reliable = logic / "08-Reliable-Flow-Transfer.md"
    require_contains(errors, reliable, "稳定一跳高频可靠 | C2", "one-hop reliable C2 contract")
    require_contains(errors, reliable, "稳定多跳高频可靠 | C4", "multi-hop reliable C4 contract")

    group = logic / "20-Group群组通信简化设计.md"
    for needle in ("Best-Effort Group", "逐成员", "UCN_ERR_UNSUPPORTED"):
        require_contains(errors, group, needle, f"Group completion/Feature-OFF contract {needle}")

    for path in logic.glob("*.md"):
        text = path.read_text(encoding="utf-8")
        if "DEINITIALIZED" in text:
            errors.append(f"stale runtime lifecycle state DEINITIALIZED: {path}")
        if "REMOTE_RECEIVED" in text or re.search(r"\bREMOTE_INBOX\b", text):
            errors.append(f"stale completion name: {path}")

    intent = proposal / "UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md"
    if intent.is_file() and "ucn_request_handle_t" in intent.read_text(encoding="utf-8"):
        errors.append(f"intent document retains a second request handle type: {intent}")


def validate_simplified_manifest(root: Path, errors: list[str]) -> None:
    manifest = (
        root / "docs" / "09-审计与整改" /
        "UCN_V6_简化文档候选清单.sha256"
    )
    if not manifest.is_file():
        errors.append(f"missing simplified document manifest: {manifest}")
        return

    proposal = root / "docs" / "10-理论与规划" / "建议方案"
    logic = proposal / "UCN_v6_逻辑模型与伪代码"
    expected = {proposal / "README.md"}
    expected.update(proposal / name for name in PROPOSAL_NAMES)
    expected.update(logic.glob("*.md"))
    expected.update(
        (proposal / "UCN_v6_V6S_00_简化版实施合同冻结").glob("*.md")
    )
    expected.update((
        root / "docs" / "09-审计与整改" /
        "UCN_V6_简化文档收口与全体自审报告_2026-09-08.md",
        root / "docs" / "00-项目管理" / "00-任务表.md",
        root / "docs" / "00-项目管理" / "01-项目操作记录.md",
        root / "docs" / "00-项目管理" / "04-UCN后续主要工作与分阶段实施路线图.md",
        root / "docs" / "00-项目管理" / "06-V6简化版分支基线与实施入口.md",
        root / "docs" / "calltree" / "README.md",
        root / "docs" / "源码阅读指南" / "06-公共函数签名索引.md",
        root / "tools" / "v6" / "check_v6_current_docs.py",
        root / "tools" / "v6" / "check_v6s_freeze_contracts.py",
        root / "tools" / "v6" / "check_v6s_wire_contract.py",
        root / "tools" / "v6" / "generate_v6_simplified_docs_manifest.py",
    ))
    expected.update(root / relative for relative in SIMPLIFIED_REMEDIATION_FILES)
    expected_relative = {
        path.resolve().relative_to(root).as_posix() for path in expected
    }

    actual_relative: set[str] = set()
    for line_number, line in enumerate(
            manifest.read_text(encoding="utf-8").splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        parts = line.split("  ", 2)
        if len(parts) != 3 or not re.fullmatch(r"[0-9A-F]{64}", parts[0]):
            errors.append(f"invalid simplified manifest line {line_number}: {manifest}")
            continue
        digest, size_text, relative = parts
        try:
            expected_size = int(size_text)
        except ValueError:
            errors.append(f"invalid manifest size at line {line_number}: {manifest}")
            continue
        path = (root / relative).resolve()
        try:
            normalized = path.relative_to(root).as_posix()
        except ValueError:
            errors.append(f"manifest path escapes root at line {line_number}: {relative}")
            continue
        if normalized in actual_relative:
            errors.append(f"duplicate simplified manifest path: {normalized}")
            continue
        actual_relative.add(normalized)
        if not path.is_file():
            errors.append(f"manifest file missing: {normalized}")
            continue
        data = path.read_bytes()
        if len(data) != expected_size:
            errors.append(f"manifest size mismatch: {normalized}")
        if hashlib.sha256(data).hexdigest().upper() != digest:
            errors.append(f"manifest hash mismatch: {normalized}")

    if actual_relative != expected_relative:
        missing = sorted(expected_relative - actual_relative)
        extra = sorted(actual_relative - expected_relative)
        errors.append(f"simplified manifest membership mismatch missing={missing} extra={extra}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    checked_links = 0
    documents = active_documents(root)
    validate_simplified_contract(root, errors)
    validate_simplified_manifest(root, errors)
    legacy_entry = root / "docs" / "01-入门与使用"
    legacy_documents = sorted(legacy_entry.rglob("*.md"))
    for document in legacy_documents:
        prefix = "\n".join(document.read_text(encoding="utf-8").splitlines()[:8])
        if "ARCHIVED / NOT CURRENT" not in prefix:
            errors.append(
                f"legacy v5 document lacks archive fence: {document.relative_to(root)}"
            )
    for document in documents:
        if not document.is_file():
            errors.append(f"missing active document: {document.relative_to(root)}")
            continue
        text = document.read_text(encoding="utf-8")
        for line_number, statement in markdown_table_findings(text):
            errors.append(
                f"markdown table column mismatch: "
                f"{document.relative_to(root)}:{line_number}: {statement}"
            )
        is_migration = document.is_relative_to(root / "docs" / "official" / "13-迁移")
        if ((document.is_relative_to(root / "docs" / "official") and not is_migration) or
                document.is_relative_to(root / "docs" / "用户手册")):
            if "Core Wire v5" in text or "UCN 5.0.0 / Core Wire v5" in text:
                errors.append(f"stale current-version marker: {document.relative_to(root)}")
        for raw_target in LINK.findall(text):
            target = raw_target.strip().split()[0].strip("<>")
            if not target or target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = unquote(target.split("#", 1)[0])
            if not target:
                continue
            candidate = (document.parent / target).resolve()
            checked_links += 1
            if not candidate.exists():
                errors.append(
                    f"broken link: {document.relative_to(root)} -> {target}"
                )
    if errors:
        for error in errors:
            print(f"V6_DOC_ERROR {error}")
        return 1
    print(
        f"V6_DOCS_OK documents={len(documents)} "
        f"legacy_fenced={len(legacy_documents)} links={checked_links}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
