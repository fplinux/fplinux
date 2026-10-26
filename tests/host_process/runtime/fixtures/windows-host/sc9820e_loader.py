# SPDX-License-Identifier: GPL-2.0-only
"""Capture RAM upload bytes before a controlled native loader readiness handshake."""

import json
import os
import sys
from pathlib import Path

directory = Path.cwd()
session = directory.parent / "session.json"
Path(os.environ["FPLINUX_TEST_ACTIVE_SESSION"]).write_text(str(session), encoding="utf-8")
Path(os.environ["FPLINUX_TEST_SC9820E_UPLOADS"]).write_text(
    json.dumps(
        {
            "argv": sys.argv[1:],
            "cwd": str(directory),
            "pid": os.getpid(),
            "image": (directory.parent / "image.bin").read_bytes().hex(),
            "files": {path.name: path.read_bytes().hex() for path in directory.iterdir()},
        }
    ),
    encoding="utf-8",
)
print("FPLINUX_BOOTROM_READY", flush=True)
if "--ready-ack" not in sys.argv or sys.stdin.buffer.read(1) != b"\x06":
    raise SystemExit(9)
print("SC9820E RAM uploads acknowledged", flush=True)
raise SystemExit(int(os.environ.get("FPLINUX_TEST_LOADER_STATUS", "0")))
