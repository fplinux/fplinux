# SPDX-License-Identifier: GPL-2.0-only
"""Observe runtime dispatch without handing off to a USB device."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING
from unittest.mock import patch

from fplinux_cli.runtime import card, cli, live, release_catalog
from fplinux_cli.runtime.events import RunLog
from fplinux_cli.runtime.state import private_directory

if TYPE_CHECKING:
    from fplinux_cli.runtime.workflow import CardInspector


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


def inspect_reader(
    bundle: Path,
    data: Path,
    logs: Path,
    *,
    inspect_card: CardInspector,
    **options: object,
) -> int:
    """Let the inspector report the SD card detected by the controlled reader."""

    def remote(
        _session: object, command: str, **_options: object
    ) -> subprocess.CompletedProcess[str]:
        if "/sys/block/mmcblk" not in command:
            msg = f"unexpected reader action: {command}"
            raise AssertionError(msg)
        return subprocess.CompletedProcess(command, 0, f"/dev/mmcblk0 {'c' * 32} 12288\n", "")

    log = RunLog(private_directory(logs), protect=private_directory)
    inspect_card((SimpleNamespace(run_remote=remote), {}), log)
    log.close("success")
    return record_action(bundle, data, logs, **options)


if __name__ == "__main__":
    archive = Path(sys.argv.pop(1))
    card_boundary = (
        patch.object(card, "run_phone", inspect_reader)
        if sys.argv[1:3] == ["card", "inspect"]
        else patch.object(card, "run_card", record_action)
    )
    with (
        patch.object(cli, "run_phone", record_action),
        patch.object(live, "console_bundle", record_action),
        card_boundary,
    ):
        release_catalog.main(archive_root=archive)
