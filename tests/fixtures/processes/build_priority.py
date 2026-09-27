# SPDX-License-Identifier: GPL-2.0-only
"""Report the native scheduling priority of a build worker and its child."""

from __future__ import annotations

import json
import os
import subprocess
import sys

from fplinux_cli.build.process import lower_build_priority

requested = int(sys.argv[1])
os.setpriority(os.PRIO_PROCESS, 0, max(requested, os.getpriority(os.PRIO_PROCESS, 0)))
before = os.getpriority(os.PRIO_PROCESS, 0)
lower_build_priority()
after = os.getpriority(os.PRIO_PROCESS, 0)
child = subprocess.check_output(
    [sys.executable, "-c", "import os; print(os.getpriority(os.PRIO_PROCESS, 0))"], text=True
)
print(json.dumps({"before": before, "after": after, "child": int(child)}))
