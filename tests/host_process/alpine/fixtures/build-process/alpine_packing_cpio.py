# SPDX-License-Identifier: GPL-2.0-only
"""Report the packing child's input, environment and requested exit status."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.stdin.buffer.read()
print(
    json.dumps(
        {
            "cwd": str(Path.cwd()),
            "locale": os.environ["LC_ALL"],
            "user": os.environ["KBUILD_BUILD_USER"],
        }
    )
)
print("packing diagnostic", file=sys.stderr)
raise SystemExit(int(os.environ["FPLINUX_CPIO_STATUS"]))
