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
        "src/core/ucn_digest.c",
        "src/core/ucn_owner.c",
        "src/core/ucn_coordinator.c",
    }
    module_scaffold_source = {"src/core/ucn_module_scaffold.c"}
    module_scaffold_sources = {
        "src/security/ucn_security_scaffold.c",
        "src/admission/ucn_admission_scaffold.c",
        "src/routing/ucn_routing_scaffold.c",
        "src/transport/ucn_transport_scaffold.c",
        "src/service/ucn_service_scaffold.c",
        "src/realtime/ucn_realtime_scaffold.c",
        "src/group/ucn_group_scaffold.c",
        "src/cluster/ucn_cluster_scaffold.c",
    }
    security_sources = {
        "src/security/ucn_security_scaffold.c",
        "src/security/ucn_security_session.c",
    }
    impl04_sources = {
        "src/admission/ucn_admission.c",
        "src/admission/ucn_identity_binding.c",
        "src/capability/ucn_capability.c",
    }
    impl05_sources = {
        "src/routing/ucn_route.c",
        "src/routing/ucn_flow.c",
    }
    impl06_sources = {
        "src/transport/ucn_transport.c",
        "src/transport/ucn_transport_codec.c",
        "src/transport/ucn_transfer.c",
        "src/transport/ucn_transport_parent.c",
    }
    impl07_sources = {
        "src/service/ucn_service.c",
        "src/service/ucn_qos.c",
        "src/service/ucn_operation.c",
        "src/service/ucn_operation_id.c",
    }
    impl08_realtime_sources = {
        "src/realtime/ucn_realtime.c",
        "src/realtime/ucn_realtime_codec.c",
        "src/realtime/ucn_realtime_domain.c",
        "src/realtime/ucn_realtime_sync.c",
        "src/realtime/ucn_realtime_policy.c",
    }
    impl08_group_sources = {
        "src/group/ucn_group.c",
        "src/group/ucn_group_send.c",
        "src/group/ucn_group_receive.c",
        "src/group/ucn_group_step.c",
    }
    impl08_cluster_sources = {
        "src/cluster/ucn_cluster.c",
        "src/cluster/ucn_cluster_config.c",
        "src/cluster/ucn_cluster_transition.c",
        "src/cluster/ucn_cluster_snapshot.c",
        "src/cluster/ucn_cluster_lineage.c",
        "src/cluster/ucn_cluster_directory.c",
        "src/cluster/ucn_cluster_persistence.c",
        "src/cluster/ucn_cluster_step.c",
    }
    impl01_sources = {
        "src/wire/ucn_wire.c",
        "src/adapter/ucn_adapter.c",
        "src/runtime/ucn_node.c",
    }
    impl09_sources = {"src/runtime/ucn_composition.c"}
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
        "src/internal/ucn_digest.h",
        "src/internal/ucn_owner.h",
        "src/internal/ucn_coordinator.h",
        "src/internal/ucn_wire.h",
        "src/internal/ucn_adapter.h",
        "src/internal/ucn_runtime.h",
        "src/internal/ucn_persistence.h",
        "src/internal/ucn_persistence_coordinator.h",
        "src/internal/ucn_module_scaffold.h",
        "src/internal/ucn_security.h",
        "src/internal/ucn_admission.h",
        "src/internal/ucn_identity.h",
        "src/internal/ucn_capability.h",
        "src/internal/ucn_route.h",
        "src/internal/ucn_flow.h",
        "src/internal/ucn_transport.h",
        "src/internal/ucn_service.h",
        "src/internal/ucn_realtime.h",
        "src/internal/ucn_group.h",
        "src/internal/ucn_cluster.h",
        "src/internal/ucn_composition.h",
    }
    actual_core_sources = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "core").glob("*.c")
    }
    actual_internal_headers = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "internal").glob("*.h")
    }
    if actual_core_sources != common_sources | module_scaffold_source:
        errors.append("src/core source inventory differs from the registered set")
    actual_impl01_sources = {
        path.relative_to(root).as_posix()
        for directory in ("wire", "adapter", "runtime")
        for path in (root / "src" / directory).glob("*.c")
    }
    if actual_impl01_sources != impl01_sources | impl09_sources:
        errors.append("IMPL-01/09 runtime source inventory differs from registry")
    actual_persistence_sources = {
        path.relative_to(root).as_posix()
        for path in (root / "src" / "persistence").glob("*.c")
    }
    if actual_persistence_sources != persistence_sources:
        errors.append("IMPL-02 source inventory differs from the registered set")
    actual_module_scaffold_sources = {
        path.relative_to(root).as_posix()
        for directory in (
            "security", "admission", "capability", "routing", "transport", "service",
            "realtime", "group", "cluster",
        )
        for path in (root / "src" / directory).glob("*.c")
    }
    if actual_module_scaffold_sources != (
        module_scaffold_sources | {"src/security/ucn_security_session.c"} |
        impl04_sources | impl05_sources | impl06_sources | impl07_sources |
        impl08_realtime_sources | impl08_group_sources |
        impl08_cluster_sources
    ):
        errors.append("IMPL-03..08 source inventory differs from registry")
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
    for relative in sorted(common_sources | module_scaffold_source |
                           module_scaffold_sources | security_sources |
                           impl01_sources | impl04_sources | impl05_sources |
                           impl06_sources | impl07_sources |
                           impl08_realtime_sources | impl08_group_sources |
                           impl08_cluster_sources | impl09_sources |
                           persistence_sources |
                           internal_headers):
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
    module_scaffold_block = cmake_block(
        cmake, "add_library", "ucn_module_scaffold")
    wire_block = cmake_block(cmake, "add_library", "ucn_wire")
    adapter_block = cmake_block(cmake, "add_library", "ucn_adapter")
    kernel_block = cmake_block(cmake, "add_library", "ucn_kernel")
    simplified_block = cmake_block(cmake, "add_library", "ucn_simplified")
    persistence_block = cmake_block(cmake, "add_library", "ucn_persistence")
    persistence_coordinator_block = cmake_block(
        cmake, "add_library", "ucn_persistence_coordinator")
    security_block = cmake_block(
        cmake, "add_library", "ucn_v6s_security_session")
    admission_block = cmake_block(
        cmake, "add_library", "ucn_v6s_admission")
    identity_block = cmake_block(
        cmake, "add_library", "ucn_v6s_identity_binding")
    capability_block = cmake_block(
        cmake, "add_library", "ucn_v6s_capability")
    route_block = cmake_block(
        cmake, "add_library", "ucn_v6s_route")
    flow_block = cmake_block(
        cmake, "add_library", "ucn_v6s_flow")
    transport_block = cmake_block(
        cmake, "add_library", "ucn_v6s_transport")
    service_block = cmake_block(
        cmake, "add_library", "ucn_v6s_service")
    realtime_block = cmake_block(
        cmake, "add_library", "ucn_v6s_realtime")
    group_block = cmake_block(
        cmake, "add_library", "ucn_v6s_group")
    cluster_block = cmake_block(
        cmake, "add_library", "ucn_v6s_cluster")
    composition_block = cmake_block(
        cmake, "add_library", "ucn_v6s_composition")
    if source_set(common_block) != {
        "src/core/ucn_checked.c",
        "src/core/ucn_digest.c",
        "src/core/ucn_owner.c",
    }:
        errors.append("ucn_common target does not own the exact common sources")
    if source_set(coordinator_block) != {"src/core/ucn_coordinator.c"}:
        errors.append("ucn_coordinator target does not own its exact source")
    if source_set(module_scaffold_block) != module_scaffold_source:
        errors.append("module scaffold target does not own its exact source")
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
    if source_set(security_block) != {"src/security/ucn_security_session.c"}:
        errors.append("Security Session target does not own its exact source")
    if source_set(admission_block) != {"src/admission/ucn_admission.c"}:
        errors.append("Admission target does not own its exact source")
    if source_set(identity_block) != {"src/admission/ucn_identity_binding.c"}:
        errors.append("Identity Binding target does not own its exact source")
    if source_set(capability_block) != {"src/capability/ucn_capability.c"}:
        errors.append("Capability target does not own its exact source")
    if source_set(route_block) != {"src/routing/ucn_route.c"}:
        errors.append("Route target does not own its exact source")
    if source_set(flow_block) != {"src/routing/ucn_flow.c"}:
        errors.append("Flow target does not own its exact source")
    if source_set(transport_block) != impl06_sources:
        errors.append("Transport target does not own the exact IMPL-06 sources")
    if source_set(service_block) != impl07_sources:
        errors.append("Service target does not own the exact IMPL-07 sources")
    if source_set(realtime_block) != impl08_realtime_sources:
        errors.append("Realtime target does not own the exact IMPL-08A sources")
    if source_set(group_block) != impl08_group_sources:
        errors.append("Group target does not own the exact IMPL-08B sources")
    if source_set(cluster_block) != impl08_cluster_sources:
        errors.append("Cluster target does not own the exact IMPL-08C/08D sources")
    if source_set(composition_block) != impl09_sources:
        errors.append("Composition target does not own the exact IMPL-09 source")

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
    security_links = cmake_block(
        cmake, "target_link_libraries", "ucn_v6s_security_session")
    if security_links is None or not re.fullmatch(
        r"\s*PRIVATE\s+ucn_common\s*", security_links
    ):
        errors.append("Security Session must depend only on Common")
    for target in (
        "ucn_v6s_admission", "ucn_v6s_identity_binding",
        "ucn_v6s_capability", "ucn_v6s_route", "ucn_v6s_flow",
        "ucn_v6s_transport", "ucn_v6s_service", "ucn_v6s_realtime",
        "ucn_v6s_group", "ucn_v6s_cluster",
    ):
        links = cmake_block(cmake, "target_link_libraries", target)
        if links is None or not re.fullmatch(
            r"\s*PRIVATE\s+ucn_common\s*", links
        ):
            errors.append(f"{target} must depend only on Common")
    composition_links = cmake_block(
        cmake, "target_link_libraries", "ucn_v6s_composition")
    if composition_links is None or not re.fullmatch(
        r"\s*PRIVATE\s+ucn_common\s+ucn_coordinator\s+ucn_kernel\s+"
        r"ucn_v6s_identity_binding\s+ucn_v6s_security_session\s+"
        r"ucn_v6s_admission\s+ucn_v6s_capability\s+ucn_v6s_route\s+"
        r"ucn_v6s_flow\s+ucn_v6s_transport\s+ucn_v6s_service\s*",
        composition_links,
    ):
        errors.append(
            "Composition lifecycle aggregator dependency set differs from registry"
        )
    for feature, target in (
        ("UCN_FEATURE_PERSISTENCE", "ucn_persistence"),
        ("UCN_FEATURE_REALTIME", "ucn_v6s_realtime"),
        ("UCN_FEATURE_GROUP", "ucn_v6s_group"),
        ("UCN_FEATURE_CLUSTER", "ucn_v6s_cluster"),
    ):
        if not re.search(
            rf"if\s*\(\s*{feature}\s*\).*?"
            rf"target_link_libraries\s*\(\s*ucn_v6s_composition\s+PRIVATE\s+"
            rf"{target}\s*\).*?endif\s*\(\s*\)",
            cmake,
            re.DOTALL,
        ):
            errors.append(
                f"Composition lacks conditional lifecycle dependency {target}"
            )

    registered_scaffolds = {
        (target, source)
        for target, source in re.findall(
            r"ucn_add_v6s_module_scaffold\s*\(\s*"
            r"(ucn_v6s_[a-z]+_scaffold)\s+"
            r"(src/[A-Za-z0-9_./-]+\.c)\s*\)",
            cmake,
        )
    }
    expected_scaffolds = {
        (f"ucn_v6s_{Path(source).parent.name}_scaffold", source)
        for source in module_scaffold_sources
    }
    if registered_scaffolds != expected_scaffolds:
        errors.append("CMake scaffold target registry differs from source registry")
    if not re.search(
        r"function\s*\(\s*ucn_add_v6s_module_scaffold\b.*?"
        r"target_link_libraries\s*\(\s*\$\{target\}\s+PRIVATE\s+"
        r"ucn_common\s+ucn_coordinator\s*\).*?endfunction\s*\(\s*\)",
        cmake,
        re.DOTALL,
    ):
        errors.append(
            "scaffold targets must depend only on Common and Coordinator"
        )

    export_block = cmake_block(cmake, "set", "UCN_EXPORT_TARGETS")
    umbrella_block = cmake_block(cmake, "target_link_libraries", "ucn")
    for target in (
        "ucn_common", "ucn_coordinator", "ucn_wire", "ucn_adapter",
        "ucn_kernel", "ucn_persistence_coordinator", "ucn_module_scaffold",
        "ucn_v6s_security_scaffold", "ucn_v6s_admission_scaffold",
        "ucn_v6s_routing_scaffold", "ucn_v6s_transport_scaffold",
        "ucn_v6s_service_scaffold", "ucn_v6s_realtime_scaffold",
        "ucn_v6s_group_scaffold", "ucn_v6s_cluster_scaffold",
        "ucn_v6s_security_session",
        "ucn_v6s_admission", "ucn_v6s_identity_binding",
        "ucn_v6s_capability", "ucn_v6s_route", "ucn_v6s_flow",
        "ucn_v6s_transport", "ucn_v6s_service", "ucn_v6s_realtime",
        "ucn_v6s_group", "ucn_v6s_cluster", "ucn_v6s_composition",
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
        f"sources={len(common_sources | module_scaffold_source | module_scaffold_sources | security_sources | impl01_sources | impl04_sources | impl05_sources | impl06_sources | impl07_sources | impl08_realtime_sources | impl08_group_sources | impl08_cluster_sources | impl09_sources | persistence_sources)} "
        f"internal_headers={len(internal_headers)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
