# SPDX-License-Identifier: GPL-2.0-only
"""Offer one reader session and update audit before yielding the geometry report."""

from __future__ import annotations

import sys
from contextlib import suppress
from pathlib import Path

with Path(sys.argv[1]).open("wb", buffering=0) as writer, suppress(BrokenPipeError):
    writer.write(b"raw pages must not be read")

with Path(sys.argv[2]).open("wb", buffering=0) as geometry:
    audit = Path(sys.argv[3])
    if audit.exists():
        audit.write_text("state=idle total_commands=2\n", encoding="ascii")
    geometry.write(b"id_bytes=2cda\nchip=UNKNOWN\ngeometry_source=unknown\n")
