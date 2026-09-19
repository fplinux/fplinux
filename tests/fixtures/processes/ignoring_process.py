# SPDX-License-Identifier: GPL-2.0-only
"""Ignore SIGTERM so the bounded test runner must kill and reap this process."""

import os
import signal
import sys
import time
from pathlib import Path


def main() -> None:
    root = Path(sys.argv[1])
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    (root / "process.pid").write_text(str(os.getpid()))
    (root / "process.ready").touch()
    time.sleep(30)


if __name__ == "__main__":
    main()
