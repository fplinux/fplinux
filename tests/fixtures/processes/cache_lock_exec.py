# SPDX-License-Identifier: GPL-2.0-only
"""Keep the inherited cache descriptor open until the parent releases it."""

import sys
import time
from pathlib import Path

acquired = Path(sys.argv[1])
release = Path(sys.argv[2])
acquired.touch()
deadline = time.monotonic() + 10
while not release.exists():
    if time.monotonic() >= deadline:
        raise RuntimeError(sys.argv[3])
    time.sleep(0.01)
