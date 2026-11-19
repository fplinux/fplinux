# SPDX-License-Identifier: GPL-2.0-only
"""Observe runtime dispatch without handing off to a USB device."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from unittest.mock import patch

from fplinux_cli.runtime import card, cli, live, release_catalog


def record_action(bundle: Path, data: Path, logs: Path, **options: object) -> int:
    """Report the inputs reaching the existing loader or connecting workflow."""
    manifest = json.loads((bundle / "runtime-manifest.json").read_bytes())
    result = {
        "target": manifest["target"],
        "profile": manifest.get("profile"),
        "bundle": bundle,
        "data": data,
        "logs": logs,
        **options,
    }
    print("RUNTIME ACTION " + json.dumps(result, default=str))
    return int(os.environ["FPLINUX_TEST_ACTION_STATUS"])


if __name__ == "__main__":
    archive = Path(sys.argv.pop(1))
    with (
        patch.object(cli, "run_phone", record_action),
        patch.object(live, "console_bundle", record_action),
        patch.object(card, "run_card", record_action),
    ):
        release_catalog.main(archive_root=archive)
