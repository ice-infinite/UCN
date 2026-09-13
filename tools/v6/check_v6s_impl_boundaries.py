#!/usr/bin/env python3
"""Fail closed when the simplified common foundation regains business coupling."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def cmake_block(text: str, command: str, first_argument: str) -> str | None:
    pattern = re.compile(
        rf"{re.escape(command)}\s*\(\s*{re.escape(first_argument)}\b"
        rf"(?P<body>.*?)\)",
        re.DOTALL,
    )
    match = pattern.search(text)
    return None if match is None else match.group("body")


def source_set(block: str | None) -> set[str]:
    if block is None:
        return set()
    return set(re.findall(r"src/[A-Za-z0-9_./-]+\.c\b", block))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    errors: list[str] = []

    common_sources = {
        "src/core/ucn_checked.c",
        "src/core/ucn_owner.c",
        "src/core/ucn_coordinator.c",
    }
    impl01_sources = {
        "src/wire/ucn_wire.c",
        "src/adapter/ucn_adapter.c",
        "src/runtime/ucn_node.c",
    }
    persistence_sources = {
        "src/persistence/ucn_persistence.c",
        "src/persistence/ucn_persistence_codec.c",
        "src/persistence/ucn_persistence_coordinator.c",
    }
    persistence_foundation_sources = {
        "src/persistence/ucn_persistence.c",
        "src/persistence/ucn_persistence_codec.c",
    }
    internal_headers = {
        "src/internal/ucn_checked.h",
        "src/internal/ucn_owner.h",
        "src/internal/ucn_coordinator.h",
        "src/internal/ucn_wire.h",
        "src/internal/ucn_adapter.h",
        "src/internal/ucn_runtime.h",
        "src/internal/ucn_persistence.h",
        "src/internal/ucn_persistence_coordinator.h",
    }
    actual_core_sources = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "core").glob("*.c")
    }
    actual_internal_headers = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "internal").glob("*.h")
    }
    if actual_core_sources != common_sources:
        errors.append("src/core source inventory differs from the registered set")
    actual_impl01_sources = {
        path.relative_to(root).as_posix()
        for directory in ("wire", "adapter", "runtime")
        for path in (root / "src" / directory).glob("*.c")
    }
    if actual_impl01_sources != impl01_sources:
        errors.append("IMPL-01 source inventory differs from the registered set")
    actual_persistence_sources = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "persistence").glob("*.c")
    }
    if actual_persistence_sources != persistence_sources:
        errors.append("IMPL-02 source inventory differs from the registered set")
    if actual_internal_headers != internal_headers:
        errors.append("src/internal header inventory differs from the registered set")

    foundation_header = (root / "src/internal/ucn_persistence.h").read_text(
        encoding="utf-8")
    if "internal/ucn_coordinator.h" in foundation_header or re.search(
        r"\b(?:ucn_i_dependency_|coordinator_route_)", foundation_header
    ):
        errors.append(
            "Persistence Foundation header regained Coordinator types or staging"
        )
    adapter_header = (
        root / "src/internal/ucn_persistence_coordinator.h"
    ).read_text(encoding="utf-8")
    if "internal/ucn_coordinator.h" not in adapter_header:
        errors.append("Persistence Coordinator adapter lacks its explicit boundary")
    for relative in persistence_foundation_sources:
        foundation_source = (root / relative).read_text(encoding="utf-8")
        if "ucn_persistence_coordinator.h" in foundation_source or re.search(
            r"\bucn_i_coordinator_", foundation_source
        ):
            errors.append(
                f"Persistence Foundation source depends on Coordinator: {relative}"
            )
    for relative in sorted(common_sources | impl01_sources |
                           persistence_sources | internal_headers):
        path = root / relative
        if not path.is_file():
            errors.append(f"missing simplified foundation file: {relative}")
            continue
        text = path.read_text(encoding="utf-8")
        if "ucn/v6/" in text or "ucn_v6_" in text:
            errors.append(f"simplified foundation depends on old v6 surface: {relative}")
        if re.search(r"\b(malloc|calloc|realloc|free|alloca)\s*\(", text):
            errors.append(f"dynamic allocation in simplified foundation: {relative}")

    public_headers = {
        "include/ucn/ucn_simplified.h",
        "include/ucn/ucn_types.h",
        "include/ucn/ucn_config.h",
        "include/ucn/ucn_driver.h",
        "include/ucn/ucn_product.h",
        "include/ucn/ucn_core.h",
        "include/ucn/ucn_persistence.h",
    }
    for relative in sorted(public_headers):
        public_header = root / relative
        if not public_header.is_file():
            errors.append(f"missing simplified public header: {relative}")
            continue
        text = public_header.read_text(encoding="utf-8")
        if "internal/" in text or "ucn/v6/" in text or "ucn_v6_" in text:
            errors.append(f"public header exposes private or old v6 surface: {relative}")

    cmake_path = root / "CMakeLists.txt"
    cmake = cmake_path.read_text(encoding="utf-8")
    common_block = cmake_block(cmake, "add_library", "ucn_common")
    coordinator_block = cmake_block(cmake, "add_library", "ucn_coordinator")
    wire_block = cmake_block(cmake, "add_library", "ucn_wire")
    adapter_block = cmake_block(cmake, "add_library", "ucn_adapter")
    kernel_block = cmake_block(cmake, "add_library", "ucn_kernel")
    simplified_block = cmake_block(cmake, "add_library", "ucn_simplified")
    persistence_block = cmake_block(cmake, "add_library", "ucn_persistence")
    persistence_coordinator_block = cmake_block(
        cmake, "add_library", "ucn_persistence_coordinator")
    if source_set(common_block) != {
        "src/core/ucn_checked.c",
        "src/core/ucn_owner.c",
    }:
        errors.append("ucn_common target does not own the exact common sources")
    if source_set(coordinator_block) != {"src/core/ucn_coordinator.c"}:
        errors.append("ucn_coordinator target does not own its exact source")
    if source_set(wire_block) != {"src/wire/ucn_wire.c"}:
        errors.append("ucn_wire target does not own its exact source")
    if source_set(adapter_block) != {"src/adapter/ucn_adapter.c"}:
        errors.append("ucn_adapter target does not own its exact source")
    if source_set(kernel_block) != {"src/runtime/ucn_node.c"}:
        errors.append("ucn_kernel target does not own its exact source")
    if source_set(simplified_block) != {
        "src/core/ucn_checked.c", "src/core/ucn_owner.c",
        "src/wire/ucn_wire.c", "src/adapter/ucn_adapter.c",
        "src/runtime/ucn_node.c",
    }:
        errors.append("ucn_simplified aggregate source set differs from registry")
    if source_set(persistence_block) != persistence_foundation_sources:
        errors.append("ucn_persistence target does not own the exact IMPL-02 sources")
    if source_set(persistence_coordinator_block) != {
        "src/persistence/ucn_persistence_coordinator.c"
    }:
        errors.append("internal Persistence Coordinator adapter source differs from registry")

    coordinator_links = cmake_block(
        cmake, "target_link_libraries", "ucn_coordinator")
    if coordinator_links is None or not re.fullmatch(
        r"\s*PRIVATE\s+ucn_common\s*", coordinator_links
    ):
        errors.append("ucn_coordinator must depend only on PRIVATE ucn_common")
    expected_links = {
        "ucn_wire": r"\s*PRIVATE\s+ucn_common\s*",
        "ucn_adapter": r"\s*PRIVATE\s+ucn_common\s*",
        "ucn_kernel": (
            r"\s*PUBLIC\s+ucn_common\s+PRIVATE\s+ucn_wire\s+ucn_adapter\s*"
        ),
    }
    for target, expected in expected_links.items():
        links = cmake_block(cmake, "target_link_libraries", target)
        if links is None or not re.fullmatch(expected, links):
            errors.append(f"{target} dependency direction differs from registry")
    persistence_coordinator_links = cmake_block(
        cmake, "target_link_libraries", "ucn_persistence_coordinator")
    if persistence_coordinator_links is None or not re.fullmatch(
        r"\s*PRIVATE\s+ucn_persistence\s+ucn_coordinator\s*",
        persistence_coordinator_links,
    ):
        errors.append(
            "Persistence Coordinator adapter must depend on Foundation and Coordinator"
        )

    export_block = cmake_block(cmake, "set", "UCN_EXPORT_TARGETS")
    umbrella_block = cmake_block(cmake, "target_link_libraries", "ucn")
    for target in (
        "ucn_common", "ucn_coordinator", "ucn_wire", "ucn_adapter",
        "ucn_kernel", "ucn_persistence_coordinator",
    ):
        if export_block is not None and re.search(rf"\b{target}\b", export_block):
            errors.append(f"internal target leaked into install export list: {target}")
        if umbrella_block is not None and re.search(rf"\b{target}\b", umbrella_block):
            errors.append(f"internal target leaked into public umbrella link: {target}")
    if export_block is None or not re.search(r"\bucn_simplified\b", export_block):
        errors.append("public ucn_simplified target is not installed/exported")
    if "list(APPEND UCN_EXPORT_TARGETS ucn_persistence)" not in cmake:
        errors.append("conditional Persistence export is not registered")
    if "if(UCN_FEATURE_PERSISTENCE)" not in cmake:
        errors.append("Persistence target/header lacks a Feature-OFF gate")
    simplified_header = (root / "include/ucn/ucn_simplified.h").read_text(
        encoding="utf-8")
    if "ucn_persistence.h" in simplified_header:
        errors.append("optional product Persistence header leaked into user umbrella")
    persistence_header = (root / "include/ucn/ucn_persistence.h").read_text(
        encoding="utf-8")
    if re.search(
        r"\bucn_(?:i_)?persistence_(?:submit|cancel|step|proof_get|"
        r"request_get|proof_retire|request_retire)\b",
        persistence_header,
    ):
        errors.append("Persistence internal Owner SPI leaked into installed header")

    for relative in sorted(public_headers):
        if relative not in cmake:
            errors.append(f"simplified public header is not installed: {relative}")
    if re.search(r"install\s*\([^)]*src/internal", cmake, re.DOTALL):
        errors.append("private internal SPI is installed")
    if "v6s_impl_boundary_gate" not in cmake:
        errors.append("simplified implementation boundary gate is not registered")

    if errors:
        for error in errors:
            print(f"V6S_IMPL_BOUNDARY_ERROR {error}")
        return 1
    print(
        "V6S_IMPL_BOUNDARY_OK "
        f"sources={len(common_sources | impl01_sources | persistence_sources)} "
        f"internal_headers={len(internal_headers)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
