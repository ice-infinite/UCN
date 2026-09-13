#!/usr/bin/env python3
"""Bound the longest statically visible synchronous call chain.

GCC's ``-fcallgraph-info=su`` output supplies both the call graph and each
function's static stack frame.  Provider/Driver callback internals are outside
the protocol archive and therefore stay in the product's separate stack
budget; every statically visible protocol frame is included here.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


NODE = re.compile(
    r'node:\s*\{\s*title:\s*"(?P<title>[^"]+)"\s*'
    r'label:\s*"(?P<label>[^"]*)"[^}]*\}'
)
EDGE = re.compile(
    r'edge:\s*\{\s*sourcename:\s*"(?P<source>[^"]+)"\s*'
    r'targetname:\s*"(?P<target>[^"]+)"'
)
STACK = re.compile(r'\\n(?P<bytes>\d+) bytes \((?P<kind>[^)]+)\)')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--limit-bytes", required=True, type=int)
    parser.add_argument("--source-prefix", action="append")
    args = parser.parse_args()

    frames: dict[str, int] = {}
    edges: dict[str, set[str]] = {}
    invalid: list[str] = []
    ci_files = sorted(args.build_dir.resolve().rglob("*.ci"))
    prefixes = [item.replace("\\", "/") for item in
                (args.source_prefix or ["/src/core/"])]

    for path in ci_files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in NODE.finditer(text):
            title = match.group("title")
            label = match.group("label").replace("\\\\", "/")
            stack = STACK.search(match.group("label"))
            if not any(prefix in label for prefix in prefixes):
                continue
            if stack is None or stack.group("kind") != "static":
                invalid.append(f"unknown or dynamic stack for {title}")
                continue
            value = int(stack.group("bytes"))
            previous = frames.get(title)
            if previous is not None and previous != value:
                invalid.append(f"conflicting stack records for {title}")
            frames[title] = value
        for match in EDGE.finditer(text):
            edges.setdefault(match.group("source"), set()).add(
                match.group("target")
            )

    if not frames:
        print(f"call-stack gate: no callgraph records found for {prefixes}")
        return 2

    memo: dict[str, tuple[int, list[str]]] = {}
    visiting: set[str] = set()

    def longest(node: str) -> tuple[int, list[str]]:
        if node in memo:
            return memo[node]
        if node in visiting:
            invalid.append(f"recursive protocol call chain at {node}")
            return (args.limit_bytes + 1, [node])
        visiting.add(node)
        best_child = (0, [])
        for target in edges.get(node, ()):
            if target not in frames:
                continue
            candidate = longest(target)
            if candidate[0] > best_child[0]:
                best_child = candidate
        visiting.remove(node)
        result = (frames[node] + best_child[0], [node] + best_child[1])
        memo[node] = result
        return result

    maximum = max((longest(node) for node in frames), key=lambda item: item[0])
    if invalid or maximum[0] > args.limit_bytes:
        for problem in invalid:
            print(f"CALL_STACK_ERROR {problem}")
        if maximum[0] > args.limit_bytes:
            print(
                f"CALL_STACK_ERROR maximum={maximum[0]} "
                f"limit={args.limit_bytes} chain={' -> '.join(maximum[1])}"
            )
        return 1
    print(
        f"CALL_STACK_OK functions={len(frames)} maximum={maximum[0]} "
        f"limit={args.limit_bytes}"
    )
    print(" -> ".join(maximum[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
