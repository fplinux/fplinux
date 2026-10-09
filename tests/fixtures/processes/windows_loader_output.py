# SPDX-License-Identifier: GPL-2.0-only
"""Supply deterministic loader diagnostics without accessing a device."""

import json
import os
import sys
from pathlib import Path

if destination := os.environ.get("FPLINUX_TEST_LOADER_ENVIRONMENT"):
    Path(destination).write_text(
        json.dumps({"LIBUSB_DEBUG": os.environ.get("LIBUSB_DEBUG")}), encoding="utf-8"
    )

print("controlled loader output", flush=True)
print("controlled loader diagnostic", file=sys.stderr, flush=True)
