# SPDX-License-Identifier: GPL-2.0-only
"""Replace the SSH process with binary writes gated by an owned local marker."""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path


def main() -> None:
    """Write two chunks or remain running for a stream cancellation test."""
    mode, release_name, pid_name, complete_name = sys.argv[1:]
    release = Path(release_name)
    Path(pid_name).write_text(str(os.getpid()), encoding="ascii")
    os.write(2, b"diagnostic filler\n" * 8192)
    os.write(1, b"\x00\xffpart")
    deadline = time.monotonic() + 10
    while not release.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if not release.exists():
        sys.exit(9)
    if mode == "stall":
        time.sleep(10)
    os.write(1, b"\x80end\n")
    Path(complete_name).touch()
    if mode == "failure":
        os.write(2, b"stream diagnostic\n")
        sys.exit(8)


if __name__ == "__main__":
    main()
