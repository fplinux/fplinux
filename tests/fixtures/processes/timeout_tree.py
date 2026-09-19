# SPDX-License-Identifier: GPL-2.0-only
"""Keep a child and descendant alive until their Stage deadline expires."""

import os
import subprocess
import sys
import time
from pathlib import Path


def main() -> None:
    root = Path(sys.argv[1])
    (root / "child.pid").write_text(str(os.getpid()))
    subprocess.Popen(
        [sys.executable, str(Path(__file__).with_name("sleeping_descendant.py")), str(root)]
    )
    while not (root / "descendant.ready").exists():
        time.sleep(0.01)
    time.sleep(30)


if __name__ == "__main__":
    main()
