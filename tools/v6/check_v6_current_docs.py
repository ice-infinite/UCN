#!/usr/bin/env python3
"""Validate links and current-version markers in the maintained v6 docs."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from urllib.parse import unquote


LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")


def active_documents(root: Path) -> list[Path]:
    docs = [root / "README.md", root / "docs" / "README.md"]
    for name in ("official", "用户手册", "源码阅读指南", "reference", "evidence", "experimental", "calltree"):
        docs.extend(sorted((root / "docs" / name).rglob("*.md")))
    management = root / "docs" / "00-项目管理"
    for name in ("00-任务表.md", "01-项目操作记录.md", "05-V6兼容删除清单.md"):
        docs.append(management / name)
    docs.extend(sorted((root / "docs" / "08-实现与验证" / "版本演进").glob("UCN_V6_*.md")))
    return sorted(set(path.resolve() for path in docs))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    checked_links = 0
    documents = active_documents(root)
    for document in documents:
        if not document.is_file():
            errors.append(f"missing active document: {document.relative_to(root)}")
            continue
        text = document.read_text(encoding="utf-8")
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
    print(f"V6_DOCS_OK documents={len(documents)} links={checked_links}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
