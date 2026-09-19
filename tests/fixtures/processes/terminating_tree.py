# SPDX-License-Identifier: GPL-2.0-only
"""Record group SIGTERM delivery and reap the descendant before exiting."""

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
    grandchild = subprocess.Popen(
        [sys.executable, str(Path(__file__).with_name("terminating_grandchild.py")), str(root)]
    )

    def terminate(_signal: int, _frame: FrameType | None) -> None:
        (root / "child.signal").touch()
        grandchild.wait(timeout=2)
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, terminate)
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
