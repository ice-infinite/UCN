#!/usr/bin/env python3
"""Exact Cargo dependency/resource trim gates, no hardware and no serial access."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
def cargo(path:str,*args:str)->str:
    r=subprocess.run([path,*args],cwd=ROOT/"rust",text=True,capture_output=True,encoding="utf-8",errors="replace")
    if r.returncode: raise RuntimeError(r.stdout+r.stderr)
    return r.stdout
def main()->None:
    sys.stdout.reconfigure(encoding="utf-8",errors="replace")
    parser=argparse.ArgumentParser();parser.add_argument("--cargo",default=shutil.which("cargo") or str(Path.home()/".cargo/bin/cargo.exe"));args=parser.parse_args()
    meta=json.loads(cargo(args.cargo,"metadata","--no-deps","--format-version","1"));packages={p["name"]:p for p in meta["packages"]}
    for name in ("ucn-realtime","ucn-group","ucn-cluster"):
        deps={d["name"] for d in packages[name]["dependencies"] if d["kind"]!="dev"}
        if deps&({"ucn-realtime","ucn-group","ucn-cluster"}-{name}):raise AssertionError(f"{name} links a sibling optional Owner")
    core=cargo(args.cargo,"tree","-p","ucn-core","--edges","normal")
    for name in ("ucn-runtime","ucn-persistence","ucn-security","ucn-transport","ucn-service","ucn-realtime","ucn-group","ucn-cluster"):
        if name in core:raise AssertionError(f"minimal Core pulls {name}")
    checks=[("ucn-transport","reliable",("ucn-persistence","ucn-durable")),("ucn-service","rpc",("ucn-persistence","ucn-durable","ucn-owner")),("ucn-service","qos",("ucn-persistence","ucn-durable","ucn-owner")),("ucn-realtime","local",("ucn-persistence","ucn-durable")),("ucn-group","static",("ucn-persistence","ucn-durable","ucn-identity"))]
    for name,features,forbidden in checks:
        tree=cargo(args.cargo,"tree","-p",name,"--no-default-features","--features",features,"--edges","normal")
        for dependency in forbidden:
            if dependency in tree:raise AssertionError(f"{name}/{features} pulls {dependency}")
        cargo(args.cargo,"check","-p",name,"--no-default-features","--features",features)
    for name in ("ucn-transport","ucn-service","ucn-realtime","ucn-group"):
        cargo(args.cargo,"check","-p",name,"--no-default-features")
    runtime_features=("","transfer","parent-control","transfer-sender","service-control","service-rpc","durable-rpc","operation-control","realtime-control","time-runtime","journal-runtime","source-runtime","duplex-runtime","capability-control","bootstrap-runtime","bootstrap-join","bootstrap-join,capability-control","peer-reauth","peer-reauth,capability-control","route-runtime","route-runtime,peer-reauth","route-data-runtime","route-data-runtime,peer-reauth")
    for features in runtime_features:
        tree=cargo(args.cargo,"tree","-p","ucn-runtime","--no-default-features","--features",features,"--edges","normal")
        forbidden={"ucn-group","ucn-cluster"}
        if features not in ("service-control","service-rpc","durable-rpc","operation-control","journal-runtime","source-runtime","duplex-runtime"):forbidden.add("ucn-service")
        if features in ("", "service-control", "service-rpc", "capability-control", "bootstrap-runtime", "bootstrap-join", "bootstrap-join,capability-control", "peer-reauth", "peer-reauth,capability-control", "route-runtime", "route-runtime,peer-reauth", "route-data-runtime", "route-data-runtime,peer-reauth"):forbidden.add("ucn-durable")
        if features in ("bootstrap-runtime", "bootstrap-join", "peer-reauth"):forbidden.add("ucn-capability")
        if features not in ("realtime-control","time-runtime"):forbidden.add("ucn-realtime")
        for dependency in forbidden:
            if dependency in tree:raise AssertionError(f"ucn-runtime/{features} pulls {dependency}")
        cargo(args.cargo,"test","-p","ucn-runtime","--no-default-features","--features",features,"--locked")
        cargo(args.cargo,"clippy","-p","ucn-runtime","--no-default-features","--features",features,"--lib","--","-D","warnings")
    print(f"RUST_OPTIONAL_BOUNDARIES_OK packages={len(packages)} feature_combinations={len(checks)+4+len(runtime_features)} isolated_optional_owners=3")
if __name__=="__main__":main()
