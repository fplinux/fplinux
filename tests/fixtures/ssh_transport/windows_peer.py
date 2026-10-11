#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-2.0-only
"""A controlled process replacing the Windows OpenSSH remote peer."""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

PUBLIC_KEY = "AAAAC3NzaC1lZDI1NTE5AAAAIHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBw"


def main() -> int:  # noqa: PLR0911 -- each branch supplies a distinct external command result.
    if Path(sys.argv[0]).name in {"keyscan", "ssh-keyscan.exe"}:
        if os.environ.get("FPLINUX_TEST_FORBID_KEYSCAN"):
            print("unexpected key scan", file=sys.stderr)
            return 8
        print(f"{sys.argv[-1]} ssh-ed25519 {PUBLIC_KEY}")
        return 0
    command = sys.argv[-1]
    if command == "binary":
        sys.stdout.buffer.write(b"\x00\r\n\xff\x80")
        print("stream diagnostic", file=sys.stderr)
        return 0
    if command == "failure":
        sys.stdout.buffer.write(b"partial")
        print("reader failed", file=sys.stderr)
        return 7
    if command.startswith("sleep:"):
        Path(command.removeprefix("sleep:")).write_text(str(os.getpid()))
        time.sleep(60)
        return 0
    if command == "fplinux-session-id; uname -r":
        if active := os.environ.get("FPLINUX_TEST_ACTIVE_SESSION"):
            session = json.loads(Path(Path(active).read_text()).read_text())
            print(session["session_id"])
        else:
            print(os.environ["FPLINUX_TEST_SESSION_ID"])
        print(os.environ["FPLINUX_TEST_KERNEL"])
        return 0
    if command.startswith("root@"):
        print("controlled shell output", flush=True)
        print("controlled shell diagnostic", file=sys.stderr, flush=True)
        if pid_file := os.environ.get("FPLINUX_TEST_SHELL_PID"):
            marker = Path(pid_file)
            pending = marker.with_suffix(".pending")
            pending.write_text(str(os.getpid()))
            pending.replace(marker)
            time.sleep(60)
        return int(os.environ.get("FPLINUX_TEST_SHELL_STATUS", "0"))
    print("unknown fixture command", file=sys.stderr)
    return 9


if __name__ == "__main__":
    raise SystemExit(main())
