# SPDX-License-Identifier: GPL-2.0-only
"""Run the incoming SSH command against a temporary phone filesystem."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def main() -> None:
    phone = Path(sys.argv[1])
    command = os.environ["SSH_ORIGINAL_COMMAND"]
    for directory in ("/proc/", "/sys/", "/run/", "/etc/", "/usr/"):
        command = command.replace(directory, f".{directory}")
    os.chdir(phone)
    os.environ["PATH"] = f"{phone / 'bin'}:{os.environ['PATH']}"
    os.execv("/bin/sh", ["sh", "-c", command])


if __name__ == "__main__":
    main()
