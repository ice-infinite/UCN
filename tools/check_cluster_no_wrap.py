#!/usr/bin/env python3
"""CLV2-13-09: deterministic Cluster safety-serial source gate.

This is deliberately narrower than a generic C linter.  It rejects direct
mutation of named Cluster safety serial fields and the historic MAX-to-1/0
wrap idioms.  Legitimate increments must go through a checked helper so the
boundary is visible to code review and runtime tests.
"""

from __future__ import annotations

import pathlib
import re
import sys


FIELDS = (
    "term",
    "config_id",
    "generation",
    "backup_generation",
    "membership_sequence",
    "snapshot_id",
    "snapshot_generation",
    "recovery_round",
    "recovery_nonce",
    "cluster_id_round",
)


def strip_comments_and_literals(text: str) -> str:
    pattern = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return pattern.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_cluster_no_wrap.py <source-root>", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    files = sorted((root / "src" / "extended").rglob("*.c"))
    field_alt = "|".join(re.escape(field) for field in FIELDS)
    direct = re.compile(
        rf"(?:->|\.)(?P<field>{field_alt})\s*(?:\+\+|--|\+=|-=)|"
        rf"(?:->|\.)(?P<lhs>{field_alt})\s*=(?!=)\s*[^;\n]{{0,80}}"
        rf"(?:->|\.)(?P=lhs)\s*\+\s*1(?:U|UL|ULL)?"
    )
    wrap = re.compile(
        rf"(?:->|\.)(?:{field_alt})[^;\n]{{0,40}}==\s*UINT32_MAX"
        r"[^;\n]{0,80}\?\s*(?:0|1)(?:U|UL|ULL)?"
    )
    failures: list[str] = []
    for path in files:
        source = path.read_text(encoding="utf-8")
        clean = strip_comments_and_literals(source)
        for expression, label in ((direct, "direct serial mutation"),
                                  (wrap, "MAX wrap idiom")):
            for match in expression.finditer(clean):
                line = clean.count("\n", 0, match.start()) + 1
                excerpt = " ".join(match.group(0).split())
                failures.append(f"{path.relative_to(root)}:{line}: {label}: {excerpt}")
    if failures:
        print("CLUSTER_NO_WRAP_GATE=FAILED")
        print("\n".join(failures))
        return 1
    print(f"CLUSTER_NO_WRAP_GATE=PASSED files={len(files)} fields={len(FIELDS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
