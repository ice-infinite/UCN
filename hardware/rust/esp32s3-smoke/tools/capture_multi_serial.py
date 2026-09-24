#!/usr/bin/env python3
"""Capture and validate the UCN Rust ESP32-S3 smoke log on multiple ports."""

from __future__ import annotations

import argparse
import json
import queue
import re
import sys
import threading
import time
from dataclasses import dataclass, asdict
from pathlib import Path

import serial


HEARTBEAT_RE = re.compile(r"UCN_RUST_HW HEARTBEAT count=(\d+) overall=(PASS|FAIL)")
SELFTEST_PASS = "UCN_RUST_HW SELFTEST wire=PASS routing=PASS flow=PASS overall=PASS"
BOOT_MARKER = "UCN_RUST_HW BOOT protocol=6 target=ESP32-S3"
FAULT_MARKERS = ("Guru Meditation", "watchdog", "abort()", "panicked at", "overall=FAIL")


@dataclass
class PortResult:
    label: str
    port: str
    opened: bool = False
    heartbeats: int = 0
    duplicate_heartbeats: int = 0
    duplicate_lines: int = 0
    boot_seen: bool = False
    first_count: int | None = None
    last_count: int | None = None
    selftest_pass_seen: bool = False
    fault_lines: int = 0
    read_error: str = ""
    require_selftest: bool = False

    @property
    def passed(self) -> bool:
        return (
            self.opened
            and (self.boot_seen or not self.require_selftest)
            and self.heartbeats >= 8
            and self.first_count is not None
            and self.last_count is not None
            and self.last_count >= self.first_count
            and (self.selftest_pass_seen or not self.require_selftest)
            and self.fault_lines == 0
            and not self.read_error
        )


def parse_port(value: str) -> tuple[str, str]:
    try:
        label, port = value.split("=", 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError("port must use LABEL=COMx") from error
    if not label or not port.upper().startswith("COM"):
        raise argparse.ArgumentTypeError("port must use LABEL=COMx")
    return label, port.upper()


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
    device.timeout = 0.2
    device.dtr = False
    device.rts = False
    try:
        device.open()
        if reset_on_open:
            # ESP32-S3 development boards expose EN through RTS and keep GPIO0
            # deasserted while DTR is false. Pulse EN only after the port is open
            # so the logger owns the complete boot/self-test transcript.
            device.reset_input_buffer()
            device.dtr = False
            device.rts = True
            time.sleep(0.1)
            device.rts = False
        messages.put((label, "OPEN", ""))
        while not stop.is_set():
            raw = device.readline()
            if not raw:
                continue
            messages.put(
                (
                    label,
                    "LINE",
                    raw.rstrip(b"\r\n").decode("utf-8", "replace"),
                )
            )
    except Exception as error:  # Hardware/driver errors must become evidence, not hide the port.
        messages.put((label, "ERROR", repr(error)))
    finally:
        if device.is_open:
            device.close()


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", action="append", type=parse_port, required=True)
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--reset-on-open", action="store_true")
    args = parser.parse_args()
    if args.seconds < 10.0:
        parser.error("--seconds must be at least 10")

    results = {
        label: PortResult(label, port, require_selftest=args.reset_on_open)
        for label, port in args.port
    }
    last_content = {label: None for label, _ in args.port}
    if len(results) != len(args.port):
        parser.error("labels must be unique")
    if len({port for _, port in args.port}) != len(args.port):
        parser.error("ports must be unique")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    messages: queue.Queue[tuple[str, str, str]] = queue.Queue()
    stop = threading.Event()
    threads = [
        threading.Thread(
            target=reader,
            args=(label, port, stop, messages, args.reset_on_open),
            daemon=True,
        )
        for label, port in args.port
    ]
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
                line = f"{elapsed:09.3f} [{label}/{result.port}] OPEN"
            elif kind == "ERROR":
                result.read_error = content
                line = f"{elapsed:09.3f} [{label}/{result.port}] ERROR {content}"
            else:
                line = f"{elapsed:09.3f} [{label}/{result.port}] {content}"
                if not content.strip():
                    result.duplicate_lines += 1
                    continue
                if content == last_content[label]:
                    result.duplicate_lines += 1
                    continue
                last_content[label] = content
                result.boot_seen |= BOOT_MARKER in content
                result.selftest_pass_seen |= SELFTEST_PASS in content
                heartbeat = HEARTBEAT_RE.search(content)
                if heartbeat:
                    if result.require_selftest and not result.boot_seen:
                        continue
                    count = int(heartbeat.group(1))
                    if result.last_count == count:
                        result.duplicate_heartbeats += 1
                        continue
                    if result.last_count is not None and count < result.last_count:
                        result.fault_lines += 1
                    result.heartbeats += 1
                    result.first_count = (
                        count if result.first_count is None else result.first_count
                    )
                    result.last_count = count
                    if heartbeat.group(2) != "PASS":
                        result.fault_lines += 1
                if any(marker in content for marker in FAULT_MARKERS):
                    result.fault_lines += 1
            print(line, flush=True)
            output.write(line + "\n")

    stop.set()
    for thread in threads:
        thread.join(timeout=2.0)
    while not messages.empty():
        label, kind, content = messages.get_nowait()
        if kind == "ERROR":
            results[label].read_error = content

    summary = {
        "schema": 1,
        "duration_s": args.seconds,
        "ports": [
            {**asdict(results[label]), "passed": results[label].passed}
            for label, _ in args.port
        ],
        "overall_pass": all(result.passed for result in results.values()),
    }
    args.summary.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if summary["overall_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
