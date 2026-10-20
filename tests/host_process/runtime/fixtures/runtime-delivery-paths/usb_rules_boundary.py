# SPDX-License-Identifier: GPL-2.0-only
"""Confine the real USB-rule setup to this delivery's owned path."""

import os
from pathlib import Path

USB_RULES_FILE = Path(os.environ["FPLINUX_AUTONOMOUS_TEST_ROOT"]) / "boundary/usb-access.rules"
