# SPDX-License-Identifier: GPL-2.0-only
"""Replace a streaming SSH command with an owned process awaiting cancellation."""

from __future__ import annotations

import os
import time
from pathlib import Path


def main() -> None:
    """Record this child, write binary data and remain alive for a bounded test."""
    os.write(1, b"\x00\xffstream started\n")
    marker = Path(os.environ["FPLINUX_STREAM_PID"])
    temporary = marker.with_suffix(".pending")
    temporary.write_text(str(os.getpid()), encoding="ascii")
    temporary.replace(marker)
    time.sleep(30)


if __name__ == "__main__":
    main()
