#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-2.0-only
"""Enter the runtime carried by this immutable FPLinux bundle."""

from __future__ import annotations

import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts"))

from fplinux_cli.runtime.cli import main  # noqa: E402

if __name__ == "__main__":
    main(bundle_root=ROOT)
