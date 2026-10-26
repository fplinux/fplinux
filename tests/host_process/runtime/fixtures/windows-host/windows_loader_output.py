# SPDX-License-Identifier: GPL-2.0-only
"""Supply deterministic loader diagnostics without accessing a device."""

import json
import os
import sys
import time
from pathlib import Path

if destination := os.environ.get("FPLINUX_TEST_LOADER_ENVIRONMENT"):
    Path(destination).write_text(
        json.dumps({"LIBUSB_DEBUG": os.environ.get("LIBUSB_DEBUG")}), encoding="utf-8"
    )
if active := os.environ.get("FPLINUX_TEST_ACTIVE_SESSION"):
    session = Path.cwd() / "session.json"
    if session.is_file():
        Path(active).write_text(str(session), encoding="utf-8")
if Path.cwd().name == "payload" and (destination := os.environ.get("FPLINUX_TEST_BRIDGE_PAYLOAD")):
    Path(destination).write_text(
        json.dumps({path.name: path.read_bytes().hex() for path in Path.cwd().iterdir()}),
        encoding="utf-8",
    )

if Path(sys.argv[0]).name == "spd_dump.exe":
    if pending := os.environ.get("FPLINUX_TEST_PENDING_LOADER_PID"):
        Path(pending).write_text(str(os.getpid()), encoding="ascii")
        time.sleep(60)
    print("FPLINUX_BOOTROM_READY", flush=True)
    if "--ready-ack" in sys.argv and sys.stdin.buffer.read(1) != b"\x06":
        raise SystemExit(1)
print("controlled loader output", flush=True)
print("controlled loader diagnostic", file=sys.stderr, flush=True)
status_name = (
    "FPLINUX_TEST_BRIDGE_STATUS" if Path.cwd().name == "payload" else "FPLINUX_TEST_LOADER_STATUS"
)
raise SystemExit(int(os.environ.get(status_name, "0")))
