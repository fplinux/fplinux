# SPDX-License-Identifier: GPL-2.0-only
"""Remain alive to be stopped, then exit when the wrapper forwards SIGCONT."""

from __future__ import annotations

import os
import signal
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import FrameType


def main() -> None:
    root = Path(sys.argv[1])

    def continue_and_exit(_signal: int, _frame: FrameType | None) -> None:
        sys.exit(0)

    signal.signal(signal.SIGCONT, continue_and_exit)
    for name, value in (("child.pgid", os.getpgrp()), ("child.pid", os.getpid())):
        pending = root / f"{name}.tmp"
        pending.write_text(str(value))
        pending.replace(root / name)
    time.sleep(30)


if __name__ == "__main__":
    main()
