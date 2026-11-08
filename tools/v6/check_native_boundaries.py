#!/usr/bin/env python3
"""Native is an optional application facade, not another protocol or Owner graph."""
from __future__ import annotations
import argparse
from pathlib import Path
import re


def forbidden(source: str) -> list[str]:
    errors = []
    # A compiler-only annotation is not an Owner dependency. Its contents are
    # validated below; no other private include is exempt.
    source = re.sub(r'^#include "internal/ucn_stack\.h"\s*$', '', source, flags=re.M)
    for pattern, reason in (
        (r'#\s*include\s*[<"](?:internal/|ucn/v6/)', "private Owner include"),
        (r'\bucn_i_[A-Za-z_]+\s*\(', "private Owner call"),
        (r'\b(?:malloc|calloc|realloc|free|alloca)\s*\(', "runtime heap"),
        (r'\b(?:ucn_i_c1_encode|ucn_i_c1_decode)\b', "second Wire codec"),
    ):
        if re.search(pattern, source): errors.append(reason)
    return errors


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    native = (root / "src/native/ucn_native.c").read_text(encoding="utf-8")
    errors = forbidden(native)
    annotation = (root/'src/internal/ucn_stack.h').read_text(encoding='utf-8')
    annotation = re.sub(r'/\*[\s\S]*?\*/', '', annotation)
    allowed = {
        '#ifndef UCN_INTERNAL_STACK_H', '#define UCN_INTERNAL_STACK_H',
        '#if defined(_MSC_VER)', '#elif defined(__GNUC__) || defined(__clang__)',
        '#else', '#endif', '#define UCN_I_STACK_PHASE',
        '#define UCN_I_STACK_PHASE __declspec(noinline)',
        '#define UCN_I_STACK_PHASE __attribute__((noinline))',
    }
    if any(line.strip() not in allowed for line in annotation.splitlines() if line.strip()):
        errors.append('stack annotation contains non-annotation code/dependency')
    for bad in ('#include "internal/ucn_security.h"', 'ucn_i_route_step();', 'malloc(4);'):
        if not forbidden(bad): errors.append("boundary mutation escaped")
    for path in (root / "src/internal/ucn_runtime.h", root / "src/runtime/ucn_node.c", root / "include/ucn/ucn_core.h"):
        if re.search(r'\bucn_native[_A-Za-z]*\b', path.read_text(encoding="utf-8")):
            errors.append(f"Core depends on Native: {path.name}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(r'option\(UCN_FEATURE_NATIVE\s+"[^"]+"\s+OFF\)', cmake): errors.append("Native is not default OFF")
    manifest = (root / "rust/crates/ucn-native/Cargo.toml").read_text(encoding="utf-8")
    if set(re.findall(r'^(ucn-[\w-]+)\s*=', manifest, re.M)) != {"ucn-core", "ucn-types", "ucn-adapter"}: errors.append("Rust Native dependency drift")
    rust = (root / "rust/crates/ucn-native/src/lib.rs").read_text(encoding="utf-8")
    if not all(s in rust for s in ("#![no_std]", "#![forbid(unsafe_code)]")): errors.append("Rust static safety contract lost")
    if re.search(r'\b(?:alloc::|std::|unsafe\s*\{)', rust): errors.append("Rust Native adds heap/std/unsafe")
    if "pub struct Health" in rust: errors.append("business DTO leaked into protocol facade")
    if errors: raise SystemExit("NATIVE_BOUNDARY_ERROR " + "; ".join(errors))
    print("NATIVE_BOUNDARY_OK private_owner_dependencies=0 wire_fields_added=0 heap_calls=0 mutations=3/3")


if __name__ == "__main__": main()
