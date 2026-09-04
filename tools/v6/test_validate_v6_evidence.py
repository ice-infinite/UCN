#!/usr/bin/env python3
"""Deterministic self-test for the fail-closed v6 evidence validator."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def invoke(validator: Path, manifest: Path, *extra: str) -> int:
    result = subprocess.run(
        [sys.executable, str(validator), str(manifest), *extra],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return result.returncode


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", type=Path, required=True)
    args = parser.parse_args()
    validator = args.validator.resolve()

    with tempfile.TemporaryDirectory(prefix="ucn-v6-evidence-") as temporary:
        root = Path(temporary)
        artifact = root / "evidence.log"
        artifact.write_bytes(b"verified\n")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        manifest = root / "manifest.json"
        base = {
            "schema": 1,
            "protocol": "UCN-v6",
            "source_commit": "1" * 40,
            "release_ready": True,
            "gates": [{
                "id": "software-matrix",
                "required": True,
                "status": "PASS",
                "artifact": "evidence.log",
                "sha256": digest,
            }],
        }
        write_json(manifest, base)
        if invoke(validator, manifest) != 0:
            return 1

        bad_digest = dict(base)
        bad_digest["gates"] = [dict(base["gates"][0], sha256="0" * 64)]
        write_json(manifest, bad_digest)
        if invoke(validator, manifest) == 0:
            return 2

        incomplete = dict(base)
        incomplete["release_ready"] = False
        incomplete["gates"] = [{
            "id": "hardware-long-soak",
            "required": True,
            "status": "HOLD",
            "artifact": "",
            "sha256": "",
        }]
        write_json(manifest, incomplete)
        if invoke(validator, manifest) != 2:
            return 3
        if invoke(validator, manifest, "--expect-incomplete") != 0:
            return 4

        false_ready = dict(incomplete)
        false_ready["release_ready"] = True
        write_json(manifest, false_ready)
        if invoke(validator, manifest) == 0:
            return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
