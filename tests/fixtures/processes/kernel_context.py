# SPDX-License-Identifier: GPL-2.0-only
"""Publish worker identity and scratch paths, then wait at a FIFO barrier."""

import os
import sys
from pathlib import Path


def main() -> None:
    root = Path(sys.argv[1])
    target = sys.argv[2]
    message = "|".join((target, str(os.getpid()), os.environ["HOME"], os.environ["TMPDIR"]))
    with (root / f"{target}.ready").open("wb", buffering=0) as ready:
        ready.write((message + "\n").encode())
    with (root / f"{target}.control").open("rb", buffering=0) as control:
        action = control.read(1)
    if action == b"F":
        raise SystemExit(23)
    if action != b"S":
        raise SystemExit(24)


if __name__ == "__main__":
    main()
