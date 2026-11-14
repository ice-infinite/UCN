#!/usr/bin/env python3
"""Reject legacy UCN symbols and archive names from a v6 build tree."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


SYMBOL = re.compile(r"\b(ucn_[A-Za-z0-9_]+)$")

SIMPLIFIED_PUBLIC = {
    "ucn_storage_required", "ucn_init", "ucn_start", "ucn_step",
    "ucn_stop", "ucn_deinit", "ucn_endpoint_add", "ucn_endpoint_remove", "ucn_endpoint_install",
    "ucn_static_path_add", "ucn_static_path_remove", "ucn_link_get",
    "ucn_send", "ucn_send_query", "ucn_send_cancel", "ucn_send_forget",
    "ucn_get_stats", "ucn_get_node_view", "ucn_get_static_binding",
    "ucn_driver_rx_publish", "ucn_driver_tx_complete",
    "ucn_driver_link_event", "ucn_callback_link_get",
    "ucn_callback_send_query", "ucn_callback_get_stats",
}

SIMPLIFIED_KERNEL_PRIVATE = {
    "ucn_i_node_tx_submit",
    "ucn_i_node_init_with_owner_instance",
    "ucn_i_node_stop_step",
}

NATIVE_PUBLIC = {
    "ucn_native_init", "ucn_native_init_backend", "ucn_native_start", "ucn_native_check_schema",
    "ucn_publisher_create", "ucn_subscription_create", "ucn_publish",
    "ucn_native_send", "ucn_publisher_close", "ucn_subscription_close",
    "ucn_subscription_quiescent", "ucn_native_source_fence", "ucn_native_step",
    "ucn_native_dispatch", "ucn_native_stop", "ucn_native_status", "ucn_native_deinit",
}
NATIVE_BACKEND_PUBLIC = {
    "ucn_native_backend_check_binding", "ucn_native_backend_enable_lifecycle",
    "ucn_native_backend_lifecycle_range", "ucn_native_backend_peer_quiescent",
    "ucn_native_backend_retire_peer",
}
NATIVE_LEAF_PREFIXES = {
    name: "ucn_i_native_" + leaf + "_"
    for leaf in ("join", "capability", "reauth")
    for name in (f"libucn_native_{leaf}.a", f"ucn_native_{leaf}.lib")
}
NATIVE_PRIVATE_PREFIXES = tuple(NATIVE_LEAF_PREFIXES.values()) + (
    "ucn_i_native_dynamic_", "ucn_i_native_route_control_",
    "ucn_i_native_routed_", "ucn_i_native_session_",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--symbol-tool", required=True)
    args = parser.parse_args()
    build_dir = Path(args.build_dir).resolve()
    # Single-config generators place archives at the build root, while Visual
    # Studio places them below the selected configuration directory.  Inspect
    # the complete build tree so this release gate has identical semantics on
    # GCC/Clang and MSVC.
    archives = sorted(build_dir.rglob("libucn*.a"))
    archives.extend(sorted(build_dir.rglob("ucn*.lib")))
    errors: list[str] = []
    composition_dependencies = {
        "ucn_storage_required", "ucn_start", "ucn_step",
        "ucn_stop", "ucn_deinit", "ucn_persistence_init_in_place",
        "ucn_persistence_deinit", "ucn_endpoint_add",
        "ucn_endpoint_remove", "ucn_static_path_add",
        "ucn_static_path_remove", "ucn_link_get", "ucn_send",
        "ucn_send_query", "ucn_send_cancel", "ucn_send_forget",
        "ucn_get_stats", "ucn_callback_link_get",
        "ucn_callback_send_query", "ucn_callback_get_stats",
        "ucn_driver_rx_publish", "ucn_driver_tx_complete",
        "ucn_driver_link_event",
    }
    optional_forbidden_dependencies = {
        "libucn_v6s_realtime.a": ("ucn_i_group_", "ucn_i_cluster_",
                                    "ucn_i_flow_", "ucn_i_persistence_",
                                    "ucn_persistence_"),
        "ucn_v6s_realtime.lib": ("ucn_i_group_", "ucn_i_cluster_",
                                   "ucn_i_flow_", "ucn_i_persistence_",
                                   "ucn_persistence_"),
        "libucn_v6s_group.a": ("ucn_i_realtime_", "ucn_i_cluster_",
                                 "ucn_i_security_", "ucn_i_route_",
                                 "ucn_i_persistence_", "ucn_persistence_"),
        "ucn_v6s_group.lib": ("ucn_i_realtime_", "ucn_i_cluster_",
                                "ucn_i_security_", "ucn_i_route_",
                                "ucn_i_persistence_", "ucn_persistence_"),
        "libucn_v6s_cluster.a": ("ucn_i_realtime_", "ucn_i_group_"),
        "ucn_v6s_cluster.lib": ("ucn_i_realtime_", "ucn_i_group_"),
    }
    if not archives:
        errors.append("no UCN static archives found")
    for archive in archives:
        is_native = archive.name in ("libucn_native.a", "ucn_native.lib")
        is_native_session = archive.name in ("libucn_native_session.a", "ucn_native_session.lib")
        is_native_routed = archive.name in ("libucn_native_routed.a", "ucn_native_routed.lib")
        native_leaf = NATIVE_LEAF_PREFIXES.get(archive.name)
        is_v6_module = (archive.name.startswith("libucn_v6_") or
                        archive.name.startswith("ucn_v6_"))
        is_simplified_common = archive.name in (
            "libucn_common.a", "ucn_common.lib",
            "libucn_coordinator.a", "ucn_coordinator.lib",
            "libucn_wire.a", "ucn_wire.lib",
            "libucn_adapter.a", "ucn_adapter.lib")
        is_simplified_kernel = archive.name in (
            "libucn_kernel.a", "ucn_kernel.lib",
            "libucn_simplified.a", "ucn_simplified.lib")
        is_persistence_adapter = archive.name in (
            "libucn_persistence_coordinator.a",
            "ucn_persistence_coordinator.lib")
        is_simplified_persistence = archive.name in (
            "libucn_persistence.a", "ucn_persistence.lib",
            "libucn_persistence_coordinator.a",
            "ucn_persistence_coordinator.lib")
        is_simplified_scaffold = (
            archive.name in (
                "libucn_module_scaffold.a", "ucn_module_scaffold.lib") or
            archive.name.startswith("libucn_v6s_") and
            archive.name.endswith("_scaffold.a") or
            archive.name.startswith("ucn_v6s_") and
            archive.name.endswith("_scaffold.lib")
        )
        is_simplified_private_module = archive.name in (
            "libucn_v6s_security_session.a",
            "ucn_v6s_security_session.lib",
            "libucn_v6s_admission.a", "ucn_v6s_admission.lib",
            "libucn_v6s_identity_binding.a", "ucn_v6s_identity_binding.lib",
            "libucn_v6s_capability.a", "ucn_v6s_capability.lib",
            "libucn_v6s_route.a", "ucn_v6s_route.lib",
            "libucn_v6s_flow.a", "ucn_v6s_flow.lib",
            "libucn_v6s_transport.a", "ucn_v6s_transport.lib",
            "libucn_v6s_service.a", "ucn_v6s_service.lib",
            "libucn_v6s_realtime.a", "ucn_v6s_realtime.lib",
            "libucn_v6s_group.a", "ucn_v6s_group.lib",
            "libucn_v6s_cluster.a", "ucn_v6s_cluster.lib",
            "libucn_v6s_composition.a", "ucn_v6s_composition.lib")
        is_simplified_composition = archive.name in (
            "libucn_v6s_composition.a", "ucn_v6s_composition.lib")
        if not (is_native or is_native_session or is_native_routed or native_leaf or is_v6_module or is_simplified_common or is_simplified_kernel
                or is_simplified_persistence or is_simplified_scaffold
                or is_simplified_private_module):
            errors.append(f"legacy archive name: {archive.name}")
            continue
        tool_name = Path(args.symbol_tool).name.lower()
        if tool_name.startswith("dumpbin"):
            command = [args.symbol_tool, "/nologo", "/linkermember:1", str(archive)]
        else:
            command = [args.symbol_tool, "-g", "--defined-only", str(archive)]
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if result.returncode != 0:
            errors.append(
                f"symbol tool failed for {archive.name}: {result.stderr.strip()}"
            )
            continue
        for line in result.stdout.splitlines():
            match = SYMBOL.search(line.strip())
            if match:
                symbol = match.group(1)
                if is_native:
                    allowed = symbol in NATIVE_PUBLIC | NATIVE_BACKEND_PUBLIC
                elif is_native_session:
                    allowed = symbol == "ucn_i_native_session_init"
                elif native_leaf:
                    allowed = symbol.startswith(native_leaf)
                elif is_native_routed:
                    allowed = (symbol in {"ucn_i_native_routed_init", "ucn_i_native_routed_fence_session",
                                          "ucn_i_native_routed_session_drained"} or
                               symbol.startswith(("ucn_i_native_route_control_", "ucn_i_native_dynamic_")))
                elif is_v6_module:
                    allowed = symbol.startswith("ucn_v6_")
                elif is_simplified_persistence:
                    allowed = (symbol.startswith("ucn_persistence_") or
                               symbol.startswith("ucn_persist_") or
                               symbol.startswith("ucn_i_persist_") or
                               symbol.startswith("ucn_i_persistence_"))
                elif is_simplified_scaffold or is_simplified_private_module:
                    allowed = (symbol.startswith("ucn_i_") or
                               (is_simplified_composition and
                                symbol in composition_dependencies))
                elif is_simplified_kernel:
                    # The installed aggregate owns both the public facade and
                    # its private implementation symbols.  The test-only
                    # kernel owns only the facade and unresolved ucn_i_ calls.
                    allowed = (symbol in SIMPLIFIED_PUBLIC or
                               symbol in SIMPLIFIED_KERNEL_PRIVATE or
                               (archive.name in (
                                   "libucn_simplified.a", "ucn_simplified.lib") and
                                symbol.startswith("ucn_i_")))
                else:
                    allowed = symbol.startswith("ucn_i_")
                if not allowed or (is_simplified_kernel and symbol.startswith(NATIVE_PRIVATE_PREFIXES)):
                    errors.append(
                        f"legacy symbol in {archive.name}: {symbol}")
        if tool_name.startswith("dumpbin"):
            undefined_command = [
                args.symbol_tool, "/nologo", "/symbols", str(archive)
            ]
        else:
            undefined_command = [args.symbol_tool, "-u", str(archive)]
        undefined = subprocess.run(
            undefined_command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if undefined.returncode != 0:
            errors.append(
                f"undefined-symbol scan failed for {archive.name}: "
                f"{undefined.stderr.strip()}"
            )
            continue
        for line in undefined.stdout.splitlines():
            if tool_name.startswith("dumpbin") and " UNDEF " not in line:
                continue
            match = SYMBOL.search(line.strip())
            if match:
                symbol = match.group(1)
                forbidden_prefixes = optional_forbidden_dependencies.get(
                    archive.name, ())
                if symbol.startswith(forbidden_prefixes):
                    errors.append(
                        f"optional-module dependency in {archive.name}: "
                        f"{symbol}")
                    continue
                if is_native:
                    allowed = symbol in SIMPLIFIED_PUBLIC
                elif is_native_routed:
                    allowed = symbol.startswith("ucn_i_") or symbol in NATIVE_BACKEND_PUBLIC
                elif is_v6_module:
                    allowed = symbol.startswith("ucn_v6_")
                elif is_simplified_persistence:
                    allowed = ((is_persistence_adapter and
                                symbol.startswith("ucn_i_")) or
                               symbol.startswith("ucn_i_persist_") or
                               symbol.startswith("ucn_i_persistence_") or
                               symbol.startswith("ucn_persistence_") or
                               symbol.startswith("ucn_persist_"))
                elif is_simplified_scaffold or is_simplified_private_module:
                    allowed = (symbol.startswith("ucn_i_") or
                               (is_simplified_composition and
                                symbol in composition_dependencies))
                elif is_simplified_kernel:
                    allowed = symbol.startswith("ucn_i_")
                else:
                    allowed = symbol.startswith("ucn_i_")
                if not allowed or (is_simplified_kernel and symbol.startswith(NATIVE_PRIVATE_PREFIXES)):
                    errors.append(
                        f"foreign UCN dependency in {archive.name}: {symbol}"
                    )
    if errors:
        for error in errors:
            print(f"V6_ARCHIVE_ERROR {error}")
        return 1
    print(f"V6_ARCHIVE_OK archives={len(archives)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
