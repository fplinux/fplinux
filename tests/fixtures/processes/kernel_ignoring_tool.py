# SPDX-License-Identifier: GPL-2.0-only
"""Report worker readiness only after a tool and grandchild can ignore SIGTERM."""

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
    target = sys.argv[2]
    worker_pid = sys.argv[3]
    (root / "tool.pid").write_text(str(os.getpid()))
    _grandchild = subprocess.Popen(
        [sys.executable, str(Path(__file__).with_name("ignoring_grandchild.py")), str(root)]
    )

    def ignore_term(_signal: int, _frame: FrameType | None) -> None:
        (root / "tool.term").touch()

    signal.signal(signal.SIGTERM, ignore_term)
    deadline = time.monotonic() + 5
    while not (root / "grandchild.ready").exists():
        if time.monotonic() >= deadline:
            message = "grandchild did not become ready"
            raise RuntimeError(message)
        time.sleep(0.01)
    message = "|".join((target, worker_pid, os.environ["HOME"], os.environ["TMPDIR"]))
    with (root / f"{target}.ready").open("wb", buffering=0) as ready:
        ready.write((message + "\n").encode())
    time.sleep(30)


if __name__ == "__main__":
    main()
