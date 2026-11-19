#!/usr/bin/env python3.14
# SPDX-License-Identifier: GPL-2.0-only
"""Enter the model selector or run an explicit release command."""

import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts"))

from fplinux_cli.runtime.release_catalog import main  # noqa: E402

if __name__ == "__main__":
    main(archive_root=ROOT)
