# SPDX-License-Identifier: GPL-2.0-only
"""Record native arguments and supply controlled JSON without interpreting PowerShell."""

import json
import os
import sys
from pathlib import Path

Path(os.environ["FPLINUX_TEST_POWERSHELL_ARGUMENTS"]).write_text(
    json.dumps(sys.argv[1:], ensure_ascii=False), encoding="utf-8"
)
environment_file = os.environ.get("FPLINUX_TEST_POWERSHELL_ENVIRONMENT")
if environment_file:
    selected_environment = {
        name: value
        for name, value in os.environ.items()
        if name.upper() == "PSMODULEPATH" or name == "FPLINUX_TEST_POWERSHELL_SENTINEL"
    }
    Path(environment_file).write_text(json.dumps(selected_environment), encoding="utf-8")
sys.stdout.write(os.environ["FPLINUX_TEST_POWERSHELL_RESULT"])
