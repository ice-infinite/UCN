#!/usr/bin/env python3
"""Capture the B-C-D Rust dynamic-routing hardware gate."""

from __future__ import annotations

import argparse
import json
import queue
import re
import statistics
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import serial


BOOT_RE = re.compile(r"UCN_RUST_ROUTE BOOT protocol=6 .* role=([BCD])")
READY_RE = re.compile(r"UCN_RUST_ROUTE READY role=([BCD]) .* baud=691200")
SHORT_BOOT_RE = re.compile(r"V6R BOOT ([BCD])")
SHORT_READY_RE = re.compile(r"V6R READY ([BCD])")
SHORT_ROUTE_RE = re.compile(r"V6R ROUTE ([BD]) (INITIAL|RECOVERED)")
SHORT_ZERO_RE = re.compile(r"V6R ZERO ([BCD])")
PING_RE = re.compile(r"UCN_RUST_ROUTE PING_OK role=([BD]) sequence=(\d+) rtt_us=(\d+)")
ROUTE_RE = re.compile(r"UCN_RUST_ROUTE ROUTE_ACTIVE role=([BD]) phase=(INITIAL|RECOVERED)")
ZERO_ERROR_TAIL_RE = re.compile(
    r"carrier_err=0 wire_err=0 route_err=0 tx_err=0 rx_err=0 timeout=0"
)
FAULT_MARKERS = (
    "Guru Meditation",
    "watchdog",
    "abort()",
    "panicked at",
    "UNSUPPORTED_BOARD",
    "_FAIL",
    "PING_TIMEOUT",
)


@dataclass
class NodeResult:
    label: str
    port: str
    opened: bool = False
    boot_seen: bool = False
    ready_seen: bool = False
    role_seen: str = ""
    route_initial_seen: bool = False
    route_recovered_seen: bool = False
    rerr_guard_seen: bool = False
    invalidation_seen: bool = False
    relay_paths_seen: bool = False
    relay_data_seen: bool = False
    zero_error_tail_seen: bool = False
    ping_sequences: list[int] = field(default_factory=list)
    rtt_us: list[int] = field(default_factory=list)
    fault_lines: int = 0
    read_error: str = ""

    def passed(self, minimum_pings: int) -> bool:
        common = (
            self.opened
            and self.boot_seen
            and self.ready_seen
            and self.role_seen == self.label
            and self.zero_error_tail_seen
            and self.fault_lines == 0
            and not self.read_error
        )
        if self.label == "C":
            return common and self.relay_paths_seen and self.relay_data_seen
        endpoint = self.route_initial_seen and len(self.rtt_us) >= minimum_pings
        if self.label == "B":
            endpoint = (
                endpoint
                and self.rerr_guard_seen
                and self.invalidation_seen
                and self.route_recovered_seen
            )
        return common and endpoint


def percentile(values: list[int], percent: int) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    index = (len(ordered) * percent + 99) // 100 - 1
    return ordered[max(0, min(index, len(ordered) - 1))]


def reader(
    label: str,
    port: str,
    stop: threading.Event,
    messages: queue.Queue[tuple[str, str, str]],
) -> None:
    device = serial.Serial()
    device.port = port
    device.baudrate = 115_200
    device.timeout = 1.5
    device.dtr = False
    device.rts = False
    try:
        device.open()
        device.reset_input_buffer()
        device.dtr = False
        device.rts = True
        time.sleep(0.1)
        device.rts = False
        messages.put((label, "OPEN", ""))
        while not stop.is_set():
            raw = device.readline()
            if raw:
                text = raw.rstrip(b"\r\n").decode("utf-8", "replace")
                messages.put((label, "LINE", text))
    except Exception as error:
        messages.put((label, "ERROR", repr(error)))
    finally:
        if device.is_open:
            device.close()


def update(result: NodeResult, content: str) -> None:
    boot = BOOT_RE.search(content)
    short_boot = SHORT_BOOT_RE.search(content)
    if boot:
        result.boot_seen = True
        result.role_seen = boot.group(1)
    elif short_boot:
        result.boot_seen = True
        result.role_seen = short_boot.group(1)
    ready = READY_RE.search(content)
    short_ready = SHORT_READY_RE.search(content)
    if ready:
        result.ready_seen = True
        # READY is emitted only after this exact image has completed boot and
        # selected its MAC-bound role. It is a stronger liveness proof than
        # the earlier BOOT line, which some FTDI captures can fragment.
        result.boot_seen = True
        result.role_seen = ready.group(1)
    elif short_ready:
        result.ready_seen = True
        result.boot_seen = True
        result.role_seen = short_ready.group(1)
    route = ROUTE_RE.search(content)
    if route is None:
        route = SHORT_ROUTE_RE.search(content)
    if route:
        result.role_seen = route.group(1)
        if route.group(2) == "INITIAL":
            result.route_initial_seen = True
        else:
            result.route_recovered_seen = True
    ping = PING_RE.search(content)
    if ping:
        result.role_seen = ping.group(1)
        sequence = int(ping.group(2))
        if sequence not in result.ping_sequences:
            result.ping_sequences.append(sequence)
            result.rtt_us.append(int(ping.group(3)))
    if "UCN_RUST_ROUTE RERR_GUARD role=B preserved=1" in content:
        result.rerr_guard_seen = True
    if "V6R RERR B" in content:
        result.rerr_guard_seen = True
    if "UCN_RUST_ROUTE ROUTE_INVALIDATED role=B removed=1 resolve_rejected=1" in content:
        result.invalidation_seen = True
    if "V6R INVALID B" in content:
        result.invalidation_seen = True
    if "UCN_RUST_ROUTE RELAY_PATHS_READY role=C routes=4" in content:
        result.relay_paths_seen = True
    if "V6R C_READY" in content:
        result.relay_paths_seen = True
    if "UCN_RUST_ROUTE RELAY_DATA_ACTIVE role=C" in content:
        result.relay_data_seen = True
    if "V6R C_DATA" in content:
        result.relay_data_seen = True
    if ZERO_ERROR_TAIL_RE.search(content):
        result.zero_error_tail_seen = True
    short_zero = SHORT_ZERO_RE.search(content)
    if short_zero:
        result.role_seen = short_zero.group(1)
        result.boot_seen = True
        result.ready_seen = True
        result.zero_error_tail_seen = True
    if any(marker in content for marker in FAULT_MARKERS):
        result.fault_lines += 1


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--b", default="COM53")
    parser.add_argument("--c", default="COM58")
    parser.add_argument("--d", default="COM56")
    parser.add_argument("--seconds", type=float, default=45.0)
    parser.add_argument("--minimum-pings", type=int, default=12)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 35:
        parser.error("--seconds must be at least 35")
    if args.minimum_pings < 8:
        parser.error("--minimum-pings must be at least 8")

    results = {
        "B": NodeResult("B", args.b.upper()),
        "C": NodeResult("C", args.c.upper()),
        "D": NodeResult("D", args.d.upper()),
    }
    stop = threading.Event()
    messages: queue.Queue[tuple[str, str, str]] = queue.Queue()
    threads = [
        threading.Thread(
            target=reader,
            args=(label, result.port, stop, messages),
            daemon=True,
        )
        for label, result in results.items()
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    for thread in threads:
        thread.start()

    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        while time.monotonic() - started < args.seconds:
            try:
                label, kind, content = messages.get(timeout=0.25)
            except queue.Empty:
                continue
            result = results[label]
            elapsed = time.monotonic() - started
            if kind == "OPEN":
                result.opened = True
                rendered = f"{elapsed:09.3f} [{label}/{result.port}] OPEN"
            elif kind == "ERROR":
                result.read_error = content
                rendered = f"{elapsed:09.3f} [{label}/{result.port}] ERROR {content}"
            else:
                update(result, content)
                rendered = f"{elapsed:09.3f} [{label}/{result.port}] {content}"
            print(rendered, flush=True)
            output.write(rendered + "\n")

    stop.set()
    for thread in threads:
        thread.join(timeout=2.0)

    nodes = []
    for label in ("B", "C", "D"):
        result = results[label]
        entry = asdict(result)
        entry["rtt_count"] = len(result.rtt_us)
        entry["rtt_min_us"] = min(result.rtt_us, default=0)
        entry["rtt_mean_us"] = (
            round(statistics.fmean(result.rtt_us), 2) if result.rtt_us else 0
        )
        entry["rtt_p50_us"] = percentile(result.rtt_us, 50)
        entry["rtt_p95_us"] = percentile(result.rtt_us, 95)
        entry["rtt_max_us"] = max(result.rtt_us, default=0)
        entry["passed"] = result.passed(args.minimum_pings)
        nodes.append(entry)
    combined = [sample for result in results.values() for sample in result.rtt_us]
    summary = {
        "schema": 1,
        "topology": "B-C-D",
        "duration_s": args.seconds,
        "minimum_pings_per_endpoint": args.minimum_pings,
        "nodes": nodes,
        "combined_rtt": {
            "count": len(combined),
            "min_us": min(combined, default=0),
            "mean_us": round(statistics.fmean(combined), 2) if combined else 0,
            "p50_us": percentile(combined, 50),
            "p95_us": percentile(combined, 95),
            "max_us": max(combined, default=0),
        },
        "overall_pass": all(node["passed"] for node in nodes),
    }
    args.summary.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if summary["overall_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
