#!/usr/bin/env python3
"""Require every public caller-storage initializer to guard aliasing."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FUNCTION_SOURCES = {
    "ucn_v6_operation_id_allocator_init_in_place": "src/v6/message/ucn_v6_message.c",
    "ucn_v6_operation_journal_init_in_place": "src/v6/message/ucn_v6_message.c",
    "ucn_v6_metric_owner_init_in_place": "src/v6/qos/ucn_v6_metric.c",
    "ucn_v6_qos_owner_init_in_place": "src/v6/qos/ucn_v6_qos.c",
    "ucn_v6_runtime_init_in_place": "src/v6/runtime/ucn_v6_runtime.c",
    "ucn_v6_realtime_owner_init_in_place": "src/v6/realtime/ucn_v6_realtime.c",
    "ucn_v6_bootstrap_owner_init_in_place": "src/v6/identity/ucn_v6_bootstrap.c",
    "ucn_v6_identity_authority_init_in_place": "src/v6/identity/ucn_v6_identity.c",
    "ucn_v6_transfer_owner_init_in_place": "src/v6/transfer/ucn_v6_transfer.c",
    "ucn_v6_stack_owner_init_in_place": "src/v6/owner/ucn_v6_owner.c",
    "ucn_v6_route_owner_init_in_place": "src/v6/route/ucn_v6_route.c",
    "ucn_v6_security_init_in_place": "src/v6/security/ucn_v6_security.c",
    "ucn_v6_adapter_init_in_place": "src/v6/adapter/ucn_v6_adapter.c",
    "ucn_v6_cluster_owner_init_in_place": "src/v6/cluster/ucn_v6_cluster.c",
    "ucn_v6_capability_owner_init_in_place": "src/v6/capability/ucn_v6_capability.c",
    "ucn_v6_freertos_port_init_in_place": "src/v6/ports/ucn_v6_freertos.c",
}

NESTED_POINTER_GUARDS = {
    "ucn_v6_operation_id_allocator_init_in_place": [
        "store->context", "callback_gate->context"],
    "ucn_v6_operation_journal_init_in_place": [
        "store->context", "store->lifecycle.context",
        "callback_gate->context"],
    "ucn_v6_runtime_init_in_place": [
        "config->adapter", "config->bootstrap", "config->security",
        "config->capability", "config->route", "config->metric",
        "config->qos", "config->transfer", "config->realtime",
        "config->cluster", "config->app.context"],
    "ucn_v6_realtime_owner_init_in_place": [
        "generation_store->context", "callback_gate->context"],
    "ucn_v6_bootstrap_owner_init_in_place": [
        "verifier->context", "callback_gate->context"],
    "ucn_v6_identity_authority_init_in_place": [
        "verifier->context", "store->context", "callback_gate->context"],
    "ucn_v6_stack_owner_init_in_place": [
        "lock_ops->context", "hooks->context"],
    "ucn_v6_security_init_in_place": [
        "store->context", "crypto->context", "callback_gate->context"],
    "ucn_v6_adapter_init_in_place": ["runtime_ops->context"],
    "ucn_v6_cluster_owner_init_in_place": [
        "authority_proof_owner->context", "store->context",
        "callback_gate->context"],
    "ucn_v6_freertos_port_init_in_place": [
        "ops->context", "stack_hooks->context"],
}


def function_body(source: str, name: str) -> str | None:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        return None
    start = source.find("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    for name, relative in FUNCTION_SOURCES.items():
        path = root / relative
        if not path.is_file():
            errors.append(f"missing source: {relative}")
            continue
        body = function_body(path.read_text(encoding="utf-8"), name)
        if body is None:
            errors.append(f"missing function: {name}")
        elif "ucn_v6_memory_ranges_overlap" not in body:
            errors.append(f"missing overlap guard: {name}")
        else:
            for token in NESTED_POINTER_GUARDS.get(name, []):
                if token not in body:
                    errors.append(f"missing nested pointer guard: {name}: {token}")
    if errors:
        for error in errors:
            print(f"V6_INIT_OVERLAP_ERROR {error}")
        return 1
    print(f"V6_INIT_OVERLAP_OK functions={len(FUNCTION_SOURCES)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
