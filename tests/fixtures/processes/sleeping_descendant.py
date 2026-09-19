# SPDX-License-Identifier: GPL-2.0-only
"""Record readiness and remain alive for the stage timeout scenario."""

import os
import sys
import time
from pathlib import Path


def main() -> None:
    root = Path(sys.argv[1])
    (root / "descendant.pid").write_text(str(os.getpid()))
    (root / "descendant.ready").touch()
    time.sleep(30)


if __name__ == "__main__":
    main()
