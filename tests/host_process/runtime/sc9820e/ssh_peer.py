# SPDX-License-Identifier: GPL-2.0-only
"""Return independent kernel identity and clock replies to real SSH consumers."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def main() -> None:
    command = sys.argv[1]
    root = Path(os.environ["SC9820E_TEST_ROOT"])
    with (root / "ssh-commands").open("a", encoding="utf-8") as output:
        output.write(command + "\n")
    if command == "uname -r":
        identity = "1" * 16 if os.environ["SC9820E_TEST_MODE"] == "wrong-identity" else "9" * 16
        print(f"7.2.9-fplinux-{identity}")
    elif command.startswith("fplinux-clock "):
        print("system=1 rtc=written")
    else:
        raise SystemExit(29)


if __name__ == "__main__":
    main()
