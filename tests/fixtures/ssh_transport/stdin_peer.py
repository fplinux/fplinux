# SPDX-License-Identifier: GPL-2.0-only
"""Replace SSH with a local child that reads stdin before returning its command result."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def main() -> int:
    """Consume inherited bytes for metadata probes and save user-command input."""
    command = sys.argv[-1]
    payload = sys.stdin.buffer.read()
    if command == "fplinux-session-id":
        print(os.environ["FPLINUX_TEST_SESSION_ID"])
    elif command == "uname -r":
        print(os.environ["FPLINUX_TEST_KERNEL"])
    elif command == "fplinux-session-id; uname -r":
        print(os.environ["FPLINUX_TEST_SESSION_ID"])
        print(os.environ["FPLINUX_TEST_KERNEL"])
    elif command == "receive-stdin":
        Path(os.environ["FPLINUX_TEST_STDIN_OUTPUT"]).write_bytes(payload)
        print("stdin received")
    else:
        print("unknown fixture command", file=sys.stderr)
        return 9
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
