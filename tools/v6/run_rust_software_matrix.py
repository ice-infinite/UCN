#!/usr/bin/env python3
"""Reproducible offline-safe Rust software matrix; never opens a COM port."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]

def source_snapshot()->dict[str,str]:
    members=sorted({*(ROOT/"rust/crates").rglob("*.rs"),*(ROOT/"rust/crates").rglob("Cargo.toml"),*(ROOT/"rust/tests").rglob("*.rs"),*(ROOT/"rust/tests").rglob("*.h"),*(ROOT/"rust/tests").rglob("*.c"),*(ROOT/"include").rglob("*.h"),*(ROOT/"src").rglob("*.h"),*(ROOT/"src").rglob("*.c"),*(ROOT/"tests/simplified").rglob("*.c"),*(ROOT/"protocol/contracts").rglob("*.json"),ROOT/"rust/Cargo.toml",ROOT/"rust/Cargo.lock",ROOT/"rust/rust-toolchain.toml",Path(__file__).resolve(),ROOT/"tools/v6/check_rust_optional_boundaries.py",ROOT/"tools/v6/check_rust08_09_codec_oracle.py",ROOT/"tools/v6/check_rust06_admission_capability_oracle.py"})
    members=sorted(set(members)|{ROOT/"CMakeLists.txt",ROOT/"tools/v6/check_v6s_impl_boundaries.py"})
    return {p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in members}
def main()->None:
    # Keep failure evidence readable even when the Windows console defaults to GBK.
    sys.stdout.reconfigure(encoding="utf-8",errors="replace")
    before=source_snapshot()
    parser=argparse.ArgumentParser();parser.add_argument("--cargo",default=shutil.which("cargo") or str(Path.home()/".cargo/bin/cargo.exe"));parser.add_argument("--skip-esp",action="store_true");args=parser.parse_args()
    commands=[("debug",[args.cargo,"test","--workspace","--locked"]),("release",[args.cargo,"test","--workspace","--release","--locked"]),("msrv-1.85",[args.cargo,"+1.85.0","test","--workspace","--locked"]),("clippy",[args.cargo,"clippy","--workspace","--all-targets","--","-D","warnings"]),("fmt",[args.cargo,"fmt","--all","--check"]),("cortex-soft",[args.cargo,"check","--workspace","--target","thumbv7em-none-eabi"]),("cortex-hard",[args.cargo,"check","--workspace","--target","thumbv7em-none-eabihf"]),("optional",[sys.executable,str(ROOT/"tools/v6/check_rust_optional_boundaries.py"),"--cargo",args.cargo]),("codec",[sys.executable,str(ROOT/"tools/v6/check_rust08_09_codec_oracle.py"),"--cargo",args.cargo]),("resources",[args.cargo,"run","--quiet","-p","ucn-runtime","--example","resources"])]
    commands.append(("admission-codec",[sys.executable,str(ROOT/"tools/v6/check_rust06_admission_capability_oracle.py")]))
    for name,profile in (("native-auth-debug",[]),("native-auth-release",["--release"])):
        commands.append((name,[args.cargo,"test","--workspace","--locked","--features","ucn-runtime/native-routed",*profile]))
    commands.append(("native-auth-msrv",[args.cargo,"+1.85.0","test","--workspace","--locked","--features","ucn-runtime/native-routed"]))
    commands.append(("native-auth-clippy",[args.cargo,"clippy","--workspace","--all-targets","--features","ucn-runtime/native-routed","--","-D","warnings"]))
    for suffix,target in (("soft","thumbv7em-none-eabi"),("hard","thumbv7em-none-eabihf")):
        commands.append(("native-auth-cortex-"+suffix,[args.cargo,"check","--workspace","--features","ucn-runtime/native-routed","--target",target]))
    if not args.skip_esp:commands.append(("esp32s3-compile-only",[args.cargo,"+esp","check","-Zbuild-std=core","--workspace","--target","xtensa-esp32s3-none-elf"]))
    environment=os.environ.copy();environment["CARGO_INCREMENTAL"]="0"
    destination=ROOT/"rust/target/software-validation";destination.mkdir(parents=True,exist_ok=True);results=[]
    for name,command in commands:
        print(f"START {name}",flush=True);p=subprocess.run(command,cwd=ROOT/"rust",env=environment,text=True,capture_output=True,encoding="utf-8",errors="replace");output=p.stdout+p.stderr
        (destination/(name+".log")).write_text(output,encoding="utf-8")
        tests=sum(int(count) for count in re.findall(r"test result: ok\. (\d+) passed",output));results.append({"name":name,"command":command,"exit":p.returncode,"tests":tests});print(f"END {name} exit={p.returncode} tests={tests}",flush=True)
        if p.returncode:print(output[-12000:],flush=True);break
    source=source_snapshot()
    stable=before==source
    report={"timestamp_utc":datetime.now(timezone.utc).isoformat(),"scope":"Host software and compile only; NO physical evidence","build_env":{"CARGO_INCREMENTAL":"0"},"source_stable":stable,"results":results,"source_sha256":source}
    (destination/"latest.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    if not stable:raise SystemExit("RUST_MATRIX_SOURCE_CHANGED: rerun without concurrent edits")
    if len(results)!=len(commands) or any(r["exit"] for r in results):raise SystemExit(1)
    print(f"RUST_SOFTWARE_MATRIX_OK configurations={len(results)} sources={len(source)} report={destination/'latest.json'}")
if __name__=="__main__":main()
