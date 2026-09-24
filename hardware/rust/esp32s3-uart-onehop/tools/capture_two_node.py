#!/usr/bin/env python3
"""Capture and validate the Rust UCN UART one-hop test on boards B and C."""

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


BOOT_RE = re.compile(r"UCN_RUST_UART BOOT protocol=6 .* role=([BC])")
READY_RE = re.compile(r"UCN_RUST_UART READY role=([BC]) .* baud=691200")
PING_RE = re.compile(r"UCN_RUST_UART PING_OK role=([BC]) sequence=(\d+) rtt_us=(\d+)")
BACKPRESSURE_RE = re.compile(r"UCN_RUST_UART BACKPRESSURE .* attempt=(\d+)")
SUMMARY_RE = re.compile(
    r"UCN_RUST_UART SUMMARY role=([BC]) tx=(\d+) rx=(\d+) uart_rx_bytes=(\d+) ping_ok=(\d+) "
    r"timeout=(\d+) rtt_min_us=(\d+) rtt_avg_us=(\d+) rtt_max_us=(\d+) "
    r"backpressure=(\d+) carrier_err=(\d+) wire_err=(\d+) seq_err=(\d+) "
    r"tx_err=(\d+) rx_err=(\d+)"
)
ERROR_TAIL_RE = re.compile(
    r"timeout=0 .*backpressure=2 carrier_err=0 wire_err=0 seq_err=0 tx_err=0 rx_err=0"
)
FAULT_MARKERS = (
    "Guru Meditation",
    "watchdog",
    "abort()",
    "panicked at",
    "UNSUPPORTED_BOARD",
    "_FAIL",
    "_ERROR",
    "PING_TIMEOUT",
    "CARRIER_REJECT",
    "WIRE_REJECT",
    "SEQUENCE_REJECT",
)


@dataclass
class NodeResult:
    label: str
    port: str
    opened: bool = False
    boot_seen: bool = False
    ready_seen: bool = False
    role_seen: str = ""
    ping_sequences: list[int] = field(default_factory=list)
    rtt_us: list[int] = field(default_factory=list)
    backpressure_events: int = 0
    summary: dict[str, int] = field(default_factory=dict)
    error_tail_seen: bool = False
    fault_lines: int = 0
    read_error: str = ""
    require_boot: bool = False
    require_backpressure: bool = False

    def passed(self, minimum_pings: int, steady: bool) -> bool:
        error_keys = ("timeout", "carrier_err", "wire_err", "seq_err", "tx_err", "rx_err")
        if steady:
            return (
                self.opened
                and self.role_seen == self.label
                and len(self.rtt_us) >= minimum_pings
                and self.error_tail_seen
                and self.fault_lines == 0
                and not self.read_error
            )
        return (
            self.opened
            and (self.boot_seen or not self.require_boot)
            and self.ready_seen
            and self.role_seen == self.label
            and len(self.rtt_us) >= minimum_pings
            and (self.backpressure_events >= 2 or not self.require_backpressure)
            and bool(self.summary)
            and self.summary.get("tx", 0) > 0
            and self.summary.get("rx", 0) > 0
            and all(self.summary.get(key, -1) == 0 for key in error_keys)
            and self.fault_lines == 0
            and not self.read_error
        )


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
    reset_on_open: bool,
) -> None:
    device = serial.Serial()
    device.port = port
    device.baudrate = 115_200
    # Some FTDI-backed boards deliver one log line in several USB bursts with
    # gaps longer than 200 ms. A short readline timeout turns one valid line
    # into unrelated fragments and makes the validator under-count events.
    device.timeout = 1.5
    device.dtr = False
    device.rts = False
    try:
        device.open()
        device.reset_input_buffer()
        if reset_on_open:
            device.dtr = False
            device.rts = True
            time.sleep(0.1)
            device.rts = False
        messages.put((label, "OPEN", ""))
        while not stop.is_set():
            raw = device.readline()
            if raw:
                messages.put((label, "LINE", raw.rstrip(b"\r\n").decode("utf-8", "replace")))
    except Exception as error:
        messages.put((label, "ERROR", repr(error)))
    finally:
        if device.is_open:
            device.close()


def update_result(result: NodeResult, content: str) -> None:
    boot = BOOT_RE.search(content)
    if boot:
        result.boot_seen = True
        result.role_seen = boot.group(1)
    ready = READY_RE.search(content)
    if ready:
        result.ready_seen = True
        result.role_seen = ready.group(1)
    ping = PING_RE.search(content)
    if ping:
        result.role_seen = ping.group(1)
        sequence = int(ping.group(2))
        if sequence not in result.ping_sequences:
            result.ping_sequences.append(sequence)
            result.rtt_us.append(int(ping.group(3)))
    if BACKPRESSURE_RE.search(content):
        result.backpressure_events += 1
    summary = SUMMARY_RE.search(content)
    if summary:
        names = (
            "tx",
            "rx",
            "uart_rx_bytes",
            "ping_ok",
            "timeout",
            "rtt_min_us",
            "rtt_avg_us",
            "rtt_max_us",
            "backpressure",
            "carrier_err",
            "wire_err",
            "seq_err",
            "tx_err",
            "rx_err",
        )
        result.summary = {
            name: int(value) for name, value in zip(names, summary.groups()[1:], strict=True)
        }
    if ERROR_TAIL_RE.search(content):
        result.error_tail_seen = True
    if any(marker in content for marker in FAULT_MARKERS):
        result.fault_lines += 1


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--b", default="COM53")
    parser.add_argument("--c", default="COM58")
    parser.add_argument("--seconds", type=float, default=35.0)
    parser.add_argument("--minimum-pings", type=int, default=20)
    parser.add_argument("--reset-on-open", action="store_true")
    parser.add_argument(
        "--steady",
        action="store_true",
        help="validate an already-running pair using PING_OK and the zero-error summary tail",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 25:
        parser.error("--seconds must be at least 25")
    if args.minimum_pings < 10:
        parser.error("--minimum-pings must be at least 10")
    if args.steady and args.reset_on_open:
        parser.error("--steady cannot be combined with --reset-on-open")

    results = {
        "B": NodeResult(
            "B",
            args.b.upper(),
            require_boot=args.reset_on_open,
            require_backpressure=args.reset_on_open,
        ),
        "C": NodeResult(
            "C",
            args.c.upper(),
            require_boot=args.reset_on_open,
            require_backpressure=args.reset_on_open,
        ),
    }
    stop = threading.Event()
    messages: queue.Queue[tuple[str, str, str]] = queue.Queue()
    threads = [
        threading.Thread(
            target=reader,
            args=(label, result.port, stop, messages, args.reset_on_open),
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
                update_result(result, content)
                rendered = f"{elapsed:09.3f} [{label}/{result.port}] {content}"
            print(rendered, flush=True)
            output.write(rendered + "\n")

    stop.set()
    for thread in threads:
        thread.join(timeout=2.0)

    nodes = []
    for label in ("B", "C"):
        result = results[label]
        entry = asdict(result)
        entry["rtt_count"] = len(result.rtt_us)
        entry["rtt_min_us"] = min(result.rtt_us, default=0)
        entry["rtt_mean_us"] = round(statistics.fmean(result.rtt_us), 2) if result.rtt_us else 0
        entry["rtt_p50_us"] = percentile(result.rtt_us, 50)
        entry["rtt_p95_us"] = percentile(result.rtt_us, 95)
        entry["rtt_max_us"] = max(result.rtt_us, default=0)
        entry["passed"] = result.passed(args.minimum_pings, args.steady)
        nodes.append(entry)
    combined = [sample for result in results.values() for sample in result.rtt_us]
    summary = {
        "schema": 1,
        "mode": "steady" if args.steady else "startup",
        "duration_s": args.seconds,
        "minimum_pings_per_node": args.minimum_pings,
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
    args.summary.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if summary["overall_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
