#!/usr/bin/env python3
"""Generate the Core reading guide function index from GCC aux-info.

The narrative reading guides explain responsibilities and call order.  This
tool supplies the exhaustive, profile-aware definition/signature inventory so
the prose does not have to duplicate hundreds of static helpers by hand.
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import re
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "源码阅读指南" / "08-函数签名与源码位置索引.md"

COMMON_SOURCES = [
    "src/core/ucn_core.c",
    "src/core/ucn_endpoint.c",
    "src/core/ucn_frame.c",
    "src/core/ucn_link_cost.c",
    "src/transport/ucn_adapter.c",
    "src/transport/ucn_event_runtime.c",
    "src/transport/ucn_standard_adapter.c",
    "src/transport/ucn_protocol_owner.c",
    "src/adapters/can/ucn_can_source.c",
    "src/adapters/stream/ucn_stream_source.c",
    "src/service/ucn_service.c",
    "src/service/ucn_service_bridge.c",
]

PROFILE_SOURCES = {
    "Nano": (
        1,
        ["src/node/ucn_node_nano.c", "src/node/ucn_profile_stubs.c"],
    ),
    "Lite": (
        2,
        ["src/node/ucn_node.c", "src/node/ucn_profile_stubs.c"],
    ),
    "Full": (
        3,
        [
            "src/node/ucn_node.c",
            "src/routing/ucn_path.c",
            "src/routing/ucn_policy.c",
        ],
    ),
}

AUX_PATTERN = re.compile(
    r"^/\* (?P<source>.+):(?P<line>\d+):NF \*/ "
    r"(?P<visibility>static|extern) (?P<signature>.*?); /\*"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gcc", default="gcc", help="GCC executable")
    parser.add_argument("--check", action="store_true", help="fail if output differs")
    return parser.parse_args()


def collect_source(gcc: str, source: str, profile: int,
                   temp_dir: pathlib.Path) -> list[dict[str, str]]:
    aux = temp_dir / (source.replace("/", "_").replace(".c", "") + f"_{profile}.aux")
    command = [
        gcc,
        "-std=c99",
        "-Iinclude",
        f"-DUCN_PROFILE={profile}",
        "-DUCN_FEATURE_SERVICE=1",
        "-aux-info",
        str(aux),
        "-fsyntax-only",
        source,
    ]
    subprocess.run(command, cwd=ROOT, check=True)
    functions: list[dict[str, str]] = []
    for raw_line in aux.read_text(encoding="utf-8", errors="replace").splitlines():
        match = AUX_PATTERN.match(raw_line)
        if match is None:
            continue
        recorded = match.group("source").replace("\\", "/")
        if recorded != source:
            continue
        signature = match.group("signature").replace("_Bool", "bool")
        functions.append({
            "source": source,
            "line": match.group("line"),
            "visibility": match.group("visibility"),
            "signature": signature,
        })
    return functions


def render_source(source: str, functions: list[dict[str, str]]) -> list[str]:
    public_count = sum(item["visibility"] == "extern" for item in functions)
    static_count = len(functions) - public_count
    lines = [
        f"### `{source}`",
        "",
        f"当前条件编译得到 **{len(functions)}** 个函数定义："
        f"公共/外部 {public_count}，静态 helper {static_count}。",
        "",
        "| 位置 | 可见性 | 编译器归一化签名 |",
        "| --- | --- | --- |",
    ]
    for item in functions:
        line = item["line"]
        location = f"[{source}:{line}](../../{source}#L{line})"
        visibility = "公共" if item["visibility"] == "extern" else "内部 static"
        signature = item["signature"].replace("`", "\\`")
        lines.append(f"| {location} | {visibility} | `{signature}` |")
    lines.append("")
    return lines


def generate(gcc: str) -> str:
    if shutil.which(gcc) is None:
        raise SystemExit(f"GCC not found: {gcc}")
    date = dt.date.today().isoformat()
    with tempfile.TemporaryDirectory(prefix="ucn_reading_index_") as temporary:
        temp_dir = pathlib.Path(temporary)
        common_results = {
            source: collect_source(gcc, source, 3, temp_dir)
            for source in COMMON_SOURCES
        }
        profile_results = {
            profile: {
                source: collect_source(gcc, source, profile_value, temp_dir)
                for source in sources
            }
            for profile, (profile_value, sources) in PROFILE_SOURCES.items()
        }

    common_count = sum(len(items) for items in common_results.values())
    profile_counts = {
        profile: sum(len(items) for items in results.values())
        for profile, results in profile_results.items()
    }
    lines = [
        "# 函数签名与源码位置索引",
        "",
        "> 本文件由 `tools/generate_core_reading_function_index.py` 使用 GCC "
        "`-aux-info` 从当前条件编译结果生成。签名是编译器归一化形式；语义、"
        "所有权和调用顺序请看本目录的公共层及三个 Profile 主文档。",
        ">",
        f"> 生成条件：当前工作树，日期：{date}，Service=ON。Service=OFF 时函数清单去掉两个 Service 源文件；Owner/Event Runtime 的函数定义仍保留，但函数体会裁掉 Service 分支。",
        "",
        "## 怎样查完整函数集合",
        "",
        f"- Nano 全集 = 公共层 {common_count} + Nano 差异层 {profile_counts['Nano']} = **{common_count + profile_counts['Nano']}** 个定义；",
        f"- Lite 全集 = 公共层 {common_count} + Lite 差异层 {profile_counts['Lite']} = **{common_count + profile_counts['Lite']}** 个定义；",
        f"- Full 全集 = 公共层 {common_count} + Full 差异层 {profile_counts['Full']} = **{common_count + profile_counts['Full']}** 个定义；",
        "- `公共` 表示源文件对外定义；`内部 static` 只能由同一翻译单元调用；",
        "- 低档 Stub 也会列为公共定义，但其行为是明确失败，不代表能力可用。",
        "",
        "## A. 三档公共层",
        "",
    ]
    for source, functions in common_results.items():
        lines.extend(render_source(source, functions))
    for profile, results in profile_results.items():
        lines.extend([f"## {profile} 差异层", ""])
        for source, functions in results.items():
            lines.extend(render_source(source, functions))
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    args = parse_arguments()
    generated = generate(args.gcc)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != generated:
            print(f"out of date: {OUTPUT.relative_to(ROOT)}")
            return 1
        print(f"up to date: {OUTPUT.relative_to(ROOT)}")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(generated, encoding="utf-8", newline="\n")
    print(f"generated: {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
