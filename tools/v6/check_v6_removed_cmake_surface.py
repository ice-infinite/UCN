#!/usr/bin/env python3
"""Prove that removed v5 CMake options and targets are not recognized."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


REMOVED_OPTIONS = (
    "UCN_BUILD_SCALE_SIM",
    "UCN_BUILD_CLUSTER_SIM",
    "UCN_BUILD_CONFIG_CONTRACT_TESTS",
    "UCN_ENABLE_WIRE_V4_RELEASE_GATES",
    "UCN_CLUSTER_ENABLE_V3_COMPAT",
    "UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL",
    "UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL",
    "UCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL",
    "UCN_FEATURE_SERVICE",
)

REMOVED_TARGETS = (
    "ucn_core",
    "ucn_cluster_wire_v3_compat",
    "ucn_cluster_wire_v4",
    "ucn_cluster_wire_dual_stack",
    "ucn_cluster_takeover_experimental",
    "ucn_cluster_handover_experimental",
    "ucn_cluster_rekey_experimental",
    "ucn_scale_sim",
    "ucn_cluster_sim",
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--generator", required=True)
    args = parser.parse_args()

    source = Path(args.source).resolve()
    work = Path(args.work).resolve()
    if work.name != "v6-removed-cmake-gate" or work == source:
        print(f"V6_CMAKE_SURFACE_ERROR unsafe work directory: {work}")
        return 1
    if work.exists():
        shutil.rmtree(work)

    configure = [
        args.cmake,
        "-S",
        str(source),
        "-B",
        str(work),
        "-G",
        args.generator,
        "-DUCN_BUILD_TESTS=OFF",
    ]
    configure.extend(f"-D{name}=ON" for name in REMOVED_OPTIONS)
    configured = run(configure)
    if configured.returncode != 0:
        print(configured.stdout)
        print("V6_CMAKE_SURFACE_ERROR nested configure failed")
        return 1
    for option in REMOVED_OPTIONS:
        if option not in configured.stdout:
            print(f"V6_CMAKE_SURFACE_ERROR removed option was consumed: {option}")
            return 1

    # `cmake --build --target help` is not implemented by Visual Studio
    # generators.  CMake emits TargetDirectories.txt for every supported
    # generator, so use that generated inventory instead of a generator-
    # specific help target.
    target_inventory = work / "CMakeFiles" / "TargetDirectories.txt"
    if not target_inventory.is_file():
        print("V6_CMAKE_SURFACE_ERROR target inventory is absent")
        return 1
    target_names = {
        Path(line.strip()).name.removesuffix(".dir")
        for line in target_inventory.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()
        if line.strip()
    }
    for target in REMOVED_TARGETS:
        if target in target_names:
            print(f"V6_CMAKE_SURFACE_ERROR removed target remains: {target}")
            return 1
    if "ucn_v6_wire" not in target_names or "ucn_v6_security" not in target_names:
        print("V6_CMAKE_SURFACE_ERROR required v6 targets are absent")
        return 1

    print(
        "V6_CMAKE_SURFACE_OK "
        f"removed_options={len(REMOVED_OPTIONS)} removed_targets={len(REMOVED_TARGETS)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
