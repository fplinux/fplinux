# SPDX-License-Identifier: GPL-2.0-only
"""Local SSH executable: controlled remote replies and a snapshot on shell entry."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main() -> None:
    """Record externally observable state when the runner replaces itself with SSH."""
    root = Path(os.environ["FPLINUX_LOADER_TEST_ROOT"])
    mode = os.environ["FPLINUX_LOADER_TEST_MODE"]
    command = sys.argv[-1]
    with (root / "ssh-commands.txt").open("a", encoding="utf-8") as trace:
        trace.write(command + "\n")
    if command == "uname -r":
        suffix = "8" * 16 if mode == "identity-failure" else "9" * 16
        print(f"6.18.42-fplinux-{suffix}")
    elif command.startswith("fplinux-clock "):
        print("system=1234 rtc=kept")
    elif "-tt" in sys.argv:
        snapshot = {
            "pid": os.getpid(),
            "events": [
                json.loads(line)
                for line in (root / "events.jsonl").read_text(encoding="utf-8").splitlines()
            ],
        }
        (root / "shell.json").write_text(json.dumps(snapshot), encoding="utf-8")
        raise SystemExit(23)
    else:
        raise SystemExit(f"unexpected SSH fixture command: {command}")


if __name__ == "__main__":
    main()
