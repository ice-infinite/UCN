#!/usr/bin/env python3
"""Validate a commit-bound UCN v6 release evidence manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VALID_STATUS = {"PASS", "HOLD", "FAIL", "NOT_APPLICABLE"}


def validate_manifest(path: Path) -> tuple[list[str], bool]:
    errors: list[str] = []
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot read manifest: {error}"], False

    if document.get("schema") != 1:
        errors.append("schema must equal 1")
    if document.get("protocol") != "UCN-v6":
        errors.append("protocol must equal UCN-v6")
    commit = document.get("source_commit")
    if not isinstance(commit, str) or SHA1_RE.fullmatch(commit) is None:
        errors.append("source_commit must be a lowercase 40-hex Git commit")
    gates = document.get("gates")
    if not isinstance(gates, list) or not gates:
        errors.append("gates must be a non-empty array")
        gates = []

    ids: set[str] = set()
    all_required_pass = True
    for index, gate in enumerate(gates):
        prefix = f"gates[{index}]"
        if not isinstance(gate, dict):
            errors.append(f"{prefix} must be an object")
            all_required_pass = False
            continue
        gate_id = gate.get("id")
        if not isinstance(gate_id, str) or not gate_id:
            errors.append(f"{prefix}.id must be non-empty")
        elif gate_id in ids:
            errors.append(f"duplicate gate id {gate_id}")
        else:
            ids.add(gate_id)
        status = gate.get("status")
        if status not in VALID_STATUS:
            errors.append(f"{prefix}.status is invalid")
        required = gate.get("required")
        if not isinstance(required, bool):
            errors.append(f"{prefix}.required must be boolean")
            required = True
        if required and status != "PASS":
            all_required_pass = False
        artifact = gate.get("artifact")
        digest = gate.get("sha256")
        if status == "PASS":
            if not isinstance(artifact, str) or not artifact:
                errors.append(f"{prefix}.artifact is required for PASS")
                continue
            artifact_path = (path.parent / artifact).resolve()
            if not artifact_path.is_file():
                errors.append(f"{prefix}.artifact does not exist")
                continue
            if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
                errors.append(f"{prefix}.sha256 must be lowercase 64-hex")
                continue
            actual = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
            if actual != digest:
                errors.append(f"{prefix}.sha256 mismatch")
        elif artifact not in (None, "") or digest not in (None, ""):
            errors.append(f"{prefix} non-PASS evidence must not name an artifact")

    release_ready = document.get("release_ready")
    if not isinstance(release_ready, bool):
        errors.append("release_ready must be boolean")
        release_ready = False
    if release_ready != all_required_pass:
        errors.append("release_ready must exactly equal all required gates PASS")
    return errors, bool(release_ready)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--expect-incomplete", action="store_true")
    args = parser.parse_args()
    errors, release_ready = validate_manifest(args.manifest.resolve())
    for error in errors:
        print(f"V6_EVIDENCE_ERROR: {error}")
    if errors:
        return 1
    if args.expect_incomplete:
        if release_ready:
            print("V6_EVIDENCE_ERROR: manifest unexpectedly claims release readiness")
            return 1
        print("V6_EVIDENCE_OK release_ready=false")
        return 0
    if not release_ready:
        print("V6_EVIDENCE_HOLD: required evidence remains incomplete")
        return 2
    print("V6_EVIDENCE_OK release_ready=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
