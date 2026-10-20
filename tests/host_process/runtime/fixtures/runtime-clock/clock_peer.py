# SPDX-License-Identifier: GPL-2.0-only
"""Return controlled clock results through a real SSH replacement process."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main() -> int:
    """Record the command while keeping protocol stdout and diagnostics separate."""
    command = sys.argv[-1]
    with Path(os.environ["FPLINUX_TEST_CLOCK_COMMANDS"]).open("a", encoding="utf-8") as stream:
        stream.write(command + "\n")
    if command.startswith("fplinux-clock "):
        reply = json.loads(Path(os.environ["FPLINUX_TEST_CLOCK_RESPONSE"]).read_text())
        sys.stdout.write(reply["stdout"])
        sys.stderr.write(reply["stderr"])
        return int(reply["status"])
    if command == "printf ready":
        print("ready", end="", flush=True)
        return 0
    print("Unexpected remote command: " + command, file=sys.stderr)
    return 9


if __name__ == "__main__":
    raise SystemExit(main())
