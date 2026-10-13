# SPDX-License-Identifier: GPL-2.0-only
"""Record native arguments and supply controlled JSON without interpreting PowerShell."""

import json
import os
import sys
from pathlib import Path

Path(os.environ["FPLINUX_TEST_POWERSHELL_ARGUMENTS"]).write_text(
    json.dumps(sys.argv[1:], ensure_ascii=False), encoding="utf-8"
)
sys.stdout.write(os.environ["FPLINUX_TEST_POWERSHELL_RESULT"])
