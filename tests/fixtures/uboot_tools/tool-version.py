#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Report the synthetic U-Boot tool identity expected by the producer."""

import sys
from pathlib import Path

if sys.argv[1:] != ["-V"]:
    raise SystemExit(1)
print(f"{Path(sys.argv[0]).name} version 2026.07")
