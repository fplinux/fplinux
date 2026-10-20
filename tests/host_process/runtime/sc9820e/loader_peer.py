#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Observe the exact uploader argv and emulate only USB re-enumeration."""

from __future__ import annotations

import json
import os
import signal
import sys
import time
from pathlib import Path


def main() -> None:
    root = Path(os.environ["SC9820E_TEST_ROOT"])
    arguments = sys.argv[1:]
    upload_directory = Path(arguments[arguments.index("fdl") + 1]).parent
    records = {
        path.stem: {"bytes": path.read_bytes().hex(), "mode": path.stat().st_mode & 0o777}
        for path in upload_directory.iterdir()
        if path.is_file()
    }
    receipt = root / "uploader.json.tmp"
    receipt.write_text(
        json.dumps(
            {
                "pid": os.getpid(),
                "arguments": arguments,
                "directory": str(upload_directory),
                "files": records,
            }
        ),
        encoding="utf-8",
    )
    receipt.replace(root / "uploader.json")
    mode = os.environ["SC9820E_TEST_MODE"]
    if mode == "cancel":
        signal.signal(signal.SIGTERM, lambda _signal, _frame: sys.exit(0))
        while True:
            time.sleep(0.1)
    if mode == "loader-failure":
        raise SystemExit(17)
    selected = root / "sysfs/3-2.6"
    if mode == "wrong-connection":
        selected.rename(root / "sysfs/3-2.7")
        selected = root / "sysfs/3-2.7"
    (selected / "idVendor").write_text("0525\n", encoding="ascii")
    (selected / "idProduct").write_text("a4a6\n", encoding="ascii")
    (selected / "devnum").write_text("42\n", encoding="ascii")
    serial = (root / "serial").read_text(encoding="ascii")
    (selected / "serial").write_text(serial, encoding="ascii")


if __name__ == "__main__":
    main()
