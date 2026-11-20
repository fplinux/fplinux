# SPDX-License-Identifier: GPL-2.0-only
"""Observe runtime dispatch without handing off to a USB device."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from contextlib import ExitStack
from dataclasses import asdict
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING
from unittest.mock import patch

from fplinux_cli.runtime import card, live, operations, release_catalog, workflow
from fplinux_cli.runtime.events import RunLog
from fplinux_cli.runtime.operations import FileTransfer
from fplinux_cli.runtime.ssh import session, transport
from fplinux_cli.runtime.state import private_directory
from fplinux_cli.runtime.windows import host as windows_host

from tests.windows_host_support import StubWindowsHost

if TYPE_CHECKING:
    from fplinux_cli.runtime.operations import BundlePaths, LiveRequest
    from fplinux_cli.runtime.workflow import CardInspector


def record_action(
    bundle: Path,
    data: Path,
    logs: Path,
    **options: object,
) -> int:
    """Report the inputs reaching the existing loader or connecting workflow."""
    manifest = json.loads((bundle / "runtime-manifest.json").read_bytes())
    connection = options.pop("connection", None)
    result = {
        "target": manifest["target"],
        "profile": manifest.get("profile"),
        "bundle": bundle,
        "data": data,
        "logs": logs,
        **options,
    }
    if isinstance(connection, tuple):
        result["selected_usb"] = connection[1]["usb_serial"]
    if isinstance(transfer := options.get("transfer"), FileTransfer):
        result["transfer"] = asdict(transfer)
    print("RUNTIME ACTION " + json.dumps(result, default=str))
    return int(os.environ["FPLINUX_TEST_ACTION_STATUS"])


def saved_binding(target: str, *, runtime_root: Path) -> dict[str, str] | None:
    """Read a controlled Linux session at the location requested by discovery."""
    saved = runtime_root / f"{target}.json"
    return json.loads(saved.read_bytes()) if saved.is_file() else None


def record_enrolled_card(request: LiveRequest, paths: BundlePaths) -> int:
    """Observe which artifacts and selection reach the common card operation."""
    return record_action(
        paths.bundle,
        paths.data,
        paths.logs,
        card_cid=request.card_cid,
        verify_only=request.verify_only,
        exec_command=request.exec_command,
    )


def connected(binding: dict[str, str]) -> bool:
    """Replace only device discovery for catalogue selection tests."""
    return binding["usb_serial"] in os.environ.get("FPLINUX_TEST_CONNECTED", "").split(",")


def windows_snapshot() -> StubWindowsHost:
    """Provide a read-only PnP snapshot and leave Windows executable admission intact."""
    serial = os.environ["FPLINUX_TEST_CONNECTED"]
    return StubWindowsHost({"devices": [{"id": f"USB\\VID_0525&PID_A4A6\\{serial}"}]})


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
        ExitStack() as platform,
        patch.object(workflow, "run_phone", record_action),
        patch.object(live, "console_bundle", record_action),
        patch.object(live, "console_card", record_enrolled_card),
        card_boundary,
    ):
        if os.environ.get("FPLINUX_TEST_WINDOWS"):
            native = SimpleNamespace(name="nt")
            platform.enter_context(patch.object(release_catalog, "os", native))
            platform.enter_context(patch.object(live, "os", native))
            platform.enter_context(patch.object(operations, "os", native))
            platform.enter_context(patch.object(windows_host, "WindowsHost", windows_snapshot))
        else:
            platform.enter_context(patch.object(session, "load_saved_session", saved_binding))
            platform.enter_context(patch.object(transport, "usb_present", connected))
        release_catalog.main(archive_root=archive)
