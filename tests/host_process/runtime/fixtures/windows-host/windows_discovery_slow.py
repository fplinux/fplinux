# SPDX-License-Identifier: GPL-2.0-only
"""A delayed Windows device query that reports the missing BootROM driver."""

import json
import os
import time
from pathlib import Path

Path(os.environ["FPLINUX_TEST_DISCOVERY_PID"]).write_text(str(os.getpid()), encoding="ascii")
time.sleep(6)
print(
    json.dumps({"devices": [{"id": "USB\\VID_1782&PID_4D00\\phone", "problem_code": 28}]}),
    flush=True,
)
