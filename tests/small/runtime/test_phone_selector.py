# SPDX-License-Identifier: GPL-2.0-only
"""Carry an explicit phone directory through source-command session consumers."""

from __future__ import annotations

import json
import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.cli import nand, runtime
from fplinux_cli.environment import definition
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import live as live_runtime
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.data_paths import checkout_runtime_root
from fplinux_cli.runtime.ssh import transport as ssh_transport
from fplinux_cli.workspace import build_inputs

from tests.small.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from tests.small.command_fixtures import CommandBundleContext


@pytest.mark.parametrize("action", ["identify", "backup"])
def test_nand_consumers_forward_the_selected_phone_to_the_bundled_runtime(
    command_bundle: CommandBundleContext, action: str
) -> None:
    """NAND readers use the selected phone's saved data."""
    selected = command_bundle.root / "second phone"
    config = {
        "nand": {},
        "runtime": {
            "usb": {
                "linux_gadget": {
                    "vendor_id": 0x0525,
                    "product_id": 0xA4A6,
                    "wait_seconds": 10,
                    "keyboard_interface": 1,
                }
            }
        },
    }
    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(targets, "load_target", return_value=config),
        mock.patch.object(nand, "load_target", return_value=config),
        mock.patch.object(
            subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as child,
    ):
        if action == "identify":
            nand.identify_target_nand("phone", phone_data=selected)
        else:
            nand.backup_target_nand("phone", command_bundle.root / "nand.bin", phone_data=selected)

    arguments = child.call_args.args[0]
    selector = arguments.index("--phone-data")
    assert arguments[selector : selector + 2] == ["--phone-data", str(selected)]
    assert arguments[arguments.index("--data-root") + 1] == str(
        checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
    )


def test_verify_selects_the_same_phone_for_build_inputs_and_authenticated_reconnect(
    command_bundle: CommandBundleContext,
) -> None:
    """An external-root build and its live comparison cannot select different phone inputs."""
    selected = command_bundle.root / "second phone"
    session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
    session = create_ready_session(session_root / "session")
    current = session_root / "session/current/phone.json"
    current.parent.mkdir(mode=0o700)
    current.write_text(json.dumps(session), encoding="utf-8")
    current.chmod(0o600)
    admitted = ValidatedBundle(
        command_bundle.bundle_path, {"target": "phone"}, "a" * 64, "9" * 64, None
    )

    def reconnect(_connection: object, _data: object, _logs: object, **options: object) -> int:
        revalidate = options["revalidate"]
        assert callable(revalidate)
        revalidate()
        assert options["phone_data"] == selected
        return 0

    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(ssh_transport, "usb_present", return_value=True),
        mock.patch.object(runtime, "admit_bundle", return_value=admitted),
        mock.patch.object(
            build_inputs, "target_workspace_snapshot", return_value=command_bundle.snapshot
        ) as capture,
        mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
        mock.patch.object(live_runtime, "console_connection", side_effect=reconnect),
    ):
        runtime.verify_booted("phone", phone_data=selected)

    assert capture.call_args == mock.call(
        "phone",
        None,
        build_type="release",
        include_device_data=True,
        phone_data=selected,
        selected_data_root=command_bundle.cache,
    )
