# SPDX-License-Identifier: GPL-2.0-only
"""Give the delivered Linux launcher a local input terminal without a real SSH peer."""

from __future__ import annotations

import os
import pty
import runpy
import sys
from pathlib import Path


def main() -> None:
    """Keep terminal ownership local while executing the unmodified archive entry point."""
    master, terminal = pty.openpty()
    try:
        os.dup2(terminal, 0)
        os.close(terminal)
        launcher = Path(__file__).with_name("Run.py")
        sys.argv[0] = str(launcher)
        runpy.run_path(str(launcher), run_name="__main__")
    finally:
        os.close(master)


if __name__ == "__main__":
    main()
