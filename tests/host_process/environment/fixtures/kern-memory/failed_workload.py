# SPDX-License-Identifier: GPL-2.0-only
"""Emit controlled foreground-workload diagnostics without exhausting memory."""

import sys
from pathlib import Path

diagnostic = (
    b"kern: the workload was killed by the kernel's OOM killer against this box's own memory cap."
)
mode = sys.argv[1]
if mode == "split":
    log_size = Path(sys.argv[2]).stat().st_size
    padding = (65536 - 17 - log_size) % 65536
    sys.stdout.buffer.write(b"." * padding)
if mode in {"exact", "split"}:
    sys.stdout.buffer.write(diagnostic + b"\n")
elif mode == "generic":
    sys.stdout.buffer.write(b"out of memory\nKilled\n")
else:
    sys.stdout.buffer.write(b"workload failed\n")
if mode == "split":
    sys.stdout.buffer.write(b"teardown output\n" * 16384)
sys.stdout.buffer.flush()
raise SystemExit(int(sys.argv[3]))
