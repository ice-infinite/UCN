#!/usr/bin/env python3
"""M14 gate: public Cluster API stays opaque and storage stays owner-only."""

from __future__ import annotations

import pathlib
import re
import sys


def fail(message: str) -> None:
    print(f"M14_CLUSTER_STORAGE_GATE: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: check_cluster_storage_boundary.py <repo-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    public = (root / "include/ucn/ucn_cluster.h").read_text(encoding="utf-8")
    storage = (root / "include/ucn/ucn_cluster_storage.h").read_text(
        encoding="utf-8"
    )
    if "typedef struct ucn_cluster ucn_cluster_t;" not in public:
        fail("ucn_cluster_t is not an opaque public handle")
    if re.search(r"struct\s+ucn_cluster\s*\{", public):
        fail("Cluster storage leaked into ucn_cluster.h")
    if "#define UCN_CLUSTER_API_VERSION ((uint16_t)2U)" not in public:
        fail("Cluster API version is not v2")
    if "struct ucn_cluster {" not in storage:
        fail("owner storage definition is missing")
    if "UCN_CLUSTER_STORAGE_LAYOUT_VERSION" not in storage:
        fail("owner storage layout version is missing")

    public_root = root / "include/ucn"
    for path in sorted(public_root.rglob("*.h")):
        if path.name == "ucn_cluster_storage.h":
            continue
        text = path.read_text(encoding="utf-8")
        if re.search(r'#\s*include\s*[<"]ucn/ucn_cluster_storage\.h[>"]', text):
            fail(f"storage leaked through public header: {path.relative_to(root)}")

    public_test = (root / "tests/test_public_headers.c").read_text(encoding="utf-8")
    if "ucn_cluster_storage.h" in public_test:
        fail("pointer-only public-header test includes Cluster storage")
    owner_test = (root / "tests/test_cluster_storage_header.c").read_text(
        encoding="utf-8"
    )
    if "ucn_cluster_storage.h" not in owner_test or "ucn_cluster_t cluster" not in owner_test:
        fail("static owner allocation proof is missing")

    print("M14_CLUSTER_STORAGE_GATE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
