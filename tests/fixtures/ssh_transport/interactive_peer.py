# SPDX-License-Identifier: GPL-2.0-only
"""Replace remote SSH with a terminal reader whose lifetime is observable."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def main() -> None:
    """Record this child and read the inherited terminal until EOF or cancellation."""
    Path(sys.argv[1]).write_text(str(os.getpid()), encoding="ascii")
    print("terminal peer ready", flush=True)
    if len(sys.argv) > 2:
        raise SystemExit(int(sys.argv[2]))
    for line in sys.stdin:
        print(f"peer received: {line.rstrip()}", flush=True)
    print("terminal peer EOF", flush=True)


if __name__ == "__main__":
    main()
