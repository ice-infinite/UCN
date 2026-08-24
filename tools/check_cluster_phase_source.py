#!/usr/bin/env python3
"""M14 source gate: Phase is the sole Cluster lifecycle state."""

from __future__ import annotations

import pathlib
import re
import sys


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return text


def fail(message: str) -> None:
    print(f"M14_PHASE_GATE: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: check_cluster_phase_source.py <repo-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    public_header = (root / "include/ucn/ucn_cluster.h").read_text(
        encoding="utf-8"
    )
    if "typedef struct ucn_cluster ucn_cluster_t;" not in public_header:
        fail("public Cluster handle is not opaque")
    header = (root / "include/ucn/ucn_cluster_storage.h").read_text(
        encoding="utf-8"
    )
    match = re.search(
        r"struct\s+ucn_cluster\s*\{(.*?)\n\}\s*;",
        header,
        flags=re.S,
    )
    if match is None:
        fail("cannot locate Cluster storage")
    runtime = code_only(match.group(1))
    retired_fields = (
        "role",
        "backup_ready",
        "backup_syncing",
        "backup_assign_pending",
        "backup_takeover_active",
        "recovery_eligible",
    )
    for field in retired_fields:
        if re.search(rf"\b{re.escape(field)}\s*;", runtime):
            fail(f"retired runtime field restored: {field}")

    retired_symbols = (
        "cluster_phase_from_legacy_state",
        "cluster_legacy_state_is_valid",
        "cluster_shadow_sync",
    )
    source_root = root / "src/extended"
    for path in sorted(source_root.rglob("*.[ch]")):
        source = code_only(path.read_text(encoding="utf-8"))
        relative = path.relative_to(root).as_posix()
        for symbol in retired_symbols:
            if re.search(rf"\b{re.escape(symbol)}\b", source):
                fail(f"retired reverse-mapping symbol in {relative}: {symbol}")
        if re.search(r"\bcluster\s*->\s*role\b", source):
            fail(f"direct runtime Role access in {relative}")

        writes = list(re.finditer(r"\bcluster\s*->\s*phase\s*=(?!=)", source))
        if not writes:
            continue
        allowed = False
        if relative == "src/extended/ucn_cluster.c":
            allowed = len(writes) == 1 and re.search(
                r"cluster\s*->\s*phase\s*=\s*cluster\s*->\s*config\.enabled",
                source,
            )
        elif relative == "src/extended/cluster/ucn_cluster_fsm.c":
            allowed = len(writes) == 1 and re.search(
                r"cluster\s*->\s*phase\s*=\s*new_phase\s*;", source
            )
        if not allowed:
            fail(f"Phase write outside init/transition owner: {relative}")

    # EXCLUDE_FROM_ALL simulators are not built by every normal target, so
    # keep their retired runtime-role reads under the same deterministic gate.
    for path in sorted((root / "tools").rglob("*.[ch]")):
        source = code_only(path.read_text(encoding="utf-8"))
        relative = path.relative_to(root).as_posix()
        if re.search(r"\.cluster\s*\.\s*role\b", source) or re.search(
            r"\bcluster\s*->\s*role\b", source
        ):
            fail(f"direct runtime Role access in {relative}")

    print("M14_PHASE_GATE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
