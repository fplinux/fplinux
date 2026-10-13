# SPDX-License-Identifier: GPL-2.0-only
"""Load a delivered event helper and inspect its open event record."""

import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("fplinux_loader_events", sys.argv[1])
assert spec is not None
assert spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
output = Path(sys.argv[2])
with module.record_events(
    output, target="nokia-ta1618", profile=None, build_type="release"
) as events:
    events.emit("waiting-for-device")
    sys.stdout.write(output.read_text(encoding="utf-8"))
