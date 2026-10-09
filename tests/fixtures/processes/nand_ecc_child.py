# SPDX-License-Identifier: GPL-2.0-only
"""Replace native BCH correction with one exact test-owned input/output mapping.

This child checks record framing and supplies controlled corrected bytes. It
does not implement BCH or establish that any codeword is mathematically valid.
"""

from __future__ import annotations

import sys
from pathlib import Path

scenario = Path(__file__).resolve().parent
if sys.stdin.buffer.read() != (scenario / "input.bin").read_bytes():
    print("unexpected BCH record stream", file=sys.stderr)
    raise SystemExit(2)

sys.stdout.buffer.write((scenario / "output.bin").read_bytes())
sys.stderr.write((scenario / "diagnostic.txt").read_text(encoding="utf-8"))
raise SystemExit(int((scenario / "exit-status.txt").read_text(encoding="ascii")))
