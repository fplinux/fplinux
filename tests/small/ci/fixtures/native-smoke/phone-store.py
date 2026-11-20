# SPDX-License-Identifier: GPL-2.0-only
"""Open the supplied target's personal store before the launcher's invitation."""

import sys

from fplinux_cli.device_data.store import locked_phone_store

with locked_phone_store(sys.argv[1]) as store:
    (store / "smoke-marker.txt").write_bytes(b"launcher phone store\n")
