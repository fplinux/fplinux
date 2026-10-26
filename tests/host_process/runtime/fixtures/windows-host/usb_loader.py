# SPDX-License-Identifier: GPL-2.0-only
"""Expose the native USB acquisition boundary through an actual child process."""

import os
import sys
import time
from pathlib import Path

assert sys.argv[1] == "--ready-ack"
scenario, directory = sys.argv[2], Path(sys.argv[3])
(directory / "child.pid").write_text(str(os.getpid()))
acknowledged = directory / "usb-acknowledged"
print("native discovery started", flush=True)
if acknowledged.exists():
    raise SystemExit(7)
if scenario == "driver-needed":
    print("native driver unsupported", file=sys.stderr, flush=True)
    raise SystemExit(2)
if scenario == "missing-ready":
    raise SystemExit(0)
if scenario == "waiting":
    (directory / "usb-opened").write_text("opened\n")
    deadline = time.monotonic() + 10
    while not (directory / "release-ready").exists():
        if time.monotonic() >= deadline:
            raise SystemExit(7)
        time.sleep(0.01)
ending = b"\r\n" if scenario == "ready-crlf" else b"\n"
sys.stdout.buffer.write(b"FPLINUX_BOOTROM_READY" + ending)
sys.stdout.buffer.flush()
if sys.stdin.buffer.read(1) != b"\x06":
    raise SystemExit(1)
(directory / "usb-transfer-started").write_text("acknowledged\n")
if scenario == "timeout":
    time.sleep(60)
    raise SystemExit(7)
deadline = time.monotonic() + 10
while not acknowledged.exists() and scenario != "waiting":
    if time.monotonic() >= deadline:
        raise SystemExit(7)
    time.sleep(0.01)
sys.stdout.buffer.write(b"FPLINUX_BOOTROM_READY" + ending)
sys.stdout.buffer.flush()
print("native transfer ended", file=sys.stderr, flush=True)
raise SystemExit(2 if scenario == "transfer-error" else 0)
