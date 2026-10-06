# SPDX-License-Identifier: GPL-2.0-only
"""Replace the external tag command with a real archive-preserving copy."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

if len(sys.argv) != 4 or sys.argv[1] != "tag":
    raise SystemExit(2)
source = Path(sys.argv[2])
destination = Path(sys.argv[3])
destination.mkdir()
result = subprocess.run(["cp", "-a", "--", str(source) + "/.", str(destination)], check=False)
raise SystemExit(result.returncode)
