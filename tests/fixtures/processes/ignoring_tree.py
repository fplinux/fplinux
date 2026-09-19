# SPDX-License-Identifier: GPL-2.0-only
"""Publish readiness after both generations can record and ignore SIGTERM."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import FrameType


def main() -> None:
    root = Path(sys.argv[1])
    (root / "child.pid").write_text(str(os.getpid()))
    _grandchild = subprocess.Popen(
        [sys.executable, str(Path(__file__).with_name("ignoring_grandchild.py")), str(root)]
    )

    def ignore_term(_signal: int, _frame: FrameType | None) -> None:
        (root / "child.term").touch()

    signal.signal(signal.SIGTERM, ignore_term)
    (root / "child.pgid").write_text(str(os.getpgrp()))
    deadline = time.monotonic() + 5
    while not (root / "grandchild.ready").exists():
        if time.monotonic() >= deadline:
            message = "grandchild did not become ready"
            raise RuntimeError(message)
        time.sleep(0.01)
    (root / "child.ready").touch()
    time.sleep(30)


if __name__ == "__main__":
    main()
