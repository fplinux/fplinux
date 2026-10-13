# SPDX-License-Identifier: GPL-2.0-only
"""Create foreign-owned files and set-id modes for the tag-copy boundary."""

from __future__ import annotations

import os
import sys
from pathlib import Path

root = Path(sys.argv[1])
directory = root / "builder"
directory.mkdir()
tool = root / "compiler"
tool.write_bytes(b"build tool\n")
os.chown(directory, 1000, 1000)
directory.chmod(0o2755)
os.chown(tool, 1000, 1000)
tool.chmod(0o6755)
