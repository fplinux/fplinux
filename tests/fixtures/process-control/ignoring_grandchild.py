# SPDX-License-Identifier: GPL-2.0-only
"""Record SIGTERM delivery without exiting, to require group escalation."""

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
    (root / "grandchild.pid").write_text(str(os.getpid()))

    def ignore_term(_signal: int, _frame: FrameType | None) -> None:
        (root / "grandchild.term").touch()

    signal.signal(signal.SIGTERM, ignore_term)
    (root / "grandchild.ready").touch()
    time.sleep(30)


if __name__ == "__main__":
    main()
