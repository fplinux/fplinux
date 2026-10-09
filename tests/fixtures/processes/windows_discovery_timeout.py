# SPDX-License-Identifier: GPL-2.0-only
"""A sleeping native-discovery stand-in with an observable owned process ID."""

import os
import time
from pathlib import Path

Path(os.environ["FPLINUX_TEST_DISCOVERY_PID"]).write_text(str(os.getpid()), encoding="ascii")
time.sleep(60)
