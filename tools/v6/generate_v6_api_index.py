#!/usr/bin/env python3
"""Generate a deterministic public v6 C function-signature index."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


DECL = re.compile(
    r"(?m)^(?:const\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*\b"
    r"(ucn_v6_[A-Za-z0-9_]+)\s*\((.*?)\)\s*;",
    re.DOTALL,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when the checked-in index differs instead of rewriting it",
    )
    args = parser.parse_args()
    root = Path(args.root).resolve()
    output = Path(args.output).resolve()
    headers = sorted((root / "include" / "ucn" / "v6").rglob("*.h"))
    lines = [
        "# 公共函数签名索引",
        "",
        "> 由 `tools/v6/generate_v6_api_index.py` 从当前公共头机械生成；不要手工编辑。",
        "",
        "每一项保留完整参数声明，用于快速定位调用入口。语义和前置条件仍以对应头文件注释",
        "与官方模块文档为准。",
        "",
    ]
    count = 0
    for header in headers:
        text = header.read_text(encoding="utf-8")
        matches = list(DECL.finditer(text))
        if not matches:
            continue
        relative = header.relative_to(root).as_posix()
        lines.extend([f"## `{relative}`", ""])
        for match in matches:
            declaration = " ".join(match.group(0).split())
            if declaration.startswith("typedef "):
                continue
            lines.extend(["```c", declaration, "```", ""])
            count += 1
    lines.extend([f"共索引 `{count}` 个公共函数。", ""])
    generated = "\n".join(lines)
    if args.check:
        if not output.is_file():
            print(f"V6_API_INDEX_MISSING output={output}")
            return 1
        if output.read_text(encoding="utf-8") != generated:
            print(f"V6_API_INDEX_STALE functions={count} output={output}")
            return 1
        print(f"V6_API_INDEX_CURRENT functions={count} output={output}")
        return 0

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generated, encoding="utf-8", newline="\n")
    print(f"V6_API_INDEX_OK functions={count} output={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
