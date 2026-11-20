# SPDX-License-Identifier: GPL-2.0-only
"""Offer one reader session and update audit before yielding the geometry report."""

from __future__ import annotations

import sys
from contextlib import ExitStack
from pathlib import Path

raw = Path(sys.argv[1])
pages = b"raw pages must not be read"
with ExitStack() as opened:
    # Opening the writer first waits for the shell's raw-device descriptor.
    with raw.open("wb", buffering=0) as writer:
        observer = opened.enter_context(raw.open("rb", buffering=0))
        writer.write(pages)

    with Path(sys.argv[2]).open("wb", buffering=0) as geometry:
        audit = Path(sys.argv[3])
        if audit.exists():
            audit.write_text("state=idle total_commands=2\n", encoding="ascii")
        geometry.write(b"id_bytes=2cda\nchip=UNKNOWN\ngeometry_source=unknown\n")

    assert sys.stdin.buffer.readline() == b"reader exited\n"
    assert observer.read() == pages, "the identification reader consumed raw NAND bytes"
