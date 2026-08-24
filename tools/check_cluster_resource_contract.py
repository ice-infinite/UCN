#!/usr/bin/env python3
"""M14 fixed-resource source gate for production Cluster code."""

from __future__ import annotations

import re
import sys
from pathlib import Path


FORBIDDEN = re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\(")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_cluster_resource_contract.py <repo-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    sources = sorted((root / "src" / "extended" / "cluster").glob("*.c"))
    sources.extend(
        path
        for path in (
            root / "src" / "extended" / "ucn_cluster.c",
            root / "src" / "extended" / "ucn_cluster_federation.c",
        )
        if path.exists()
    )
    violations: list[str] = []
    for path in sources:
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if FORBIDDEN.search(line):
                violations.append(f"{path.relative_to(root)}:{line_number}:{line.strip()}")
    if violations:
        print("Cluster resource contract FAILED: dynamic allocation in production scope")
        print("\n".join(violations))
        return 1
    print(
        "Cluster resource contract PASSED: "
        f"files={len(sources)} dynamic_allocations=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
