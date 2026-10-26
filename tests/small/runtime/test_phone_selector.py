# SPDX-License-Identifier: GPL-2.0-only
"""Carry an explicit phone directory through source-command session consumers."""

from __future__ import annotations

import os
import pwd
import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.cli import nand, runtime
from fplinux_cli.environment import definition
from fplinux_cli.manifests import targets
from fplinux_cli.workspace import build_inputs

from tests.small.build.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.build.command_fixtures import CommandBundleContext


@pytest.mark.parametrize("action", ["console", "identify", "backup", "keyboard", "sudo-keyboard"])
def test_source_session_consumers_forward_the_selected_phone_to_the_bundled_runtime(
    command_bundle: CommandBundleContext, action: str
) -> None:
    """Console, reader and keyboard authentication use the selected phone's saved data."""
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
    account = pwd.struct_passwd(("caller", "x", 1234, 2345, "", "/home/caller", "/bin/sh"))
    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(targets, "load_target", return_value=config),
        mock.patch.object(nand, "load_target", return_value=config),
        mock.patch.object(os, "geteuid", return_value=0 if action == "sudo-keyboard" else 1000),
        mock.patch.dict(os.environ, {"SUDO_UID": "1234", "SUDO_USER": "caller"}),
        mock.patch.object(pwd, "getpwuid", return_value=account),
        mock.patch.object(
            subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as child,
        mock.patch.object(os, "execv"),
    ):
        if action == "identify":
            nand.identify_target_nand("phone", phone_data=selected)
        elif action == "backup":
            nand.backup_target_nand("phone", command_bundle.root / "nand.bin", phone_data=selected)
        else:
            runtime.console_target(
                "phone",
                keyboard="/dev/input/event1" if "keyboard" in action else None,
                exec_command=None,
                upload=None,
                pull=None,
                phone_data=selected,
            )

    arguments = child.call_args.args[0]
    selector = arguments.index("--phone-data")
    assert arguments[selector : selector + 2] == ["--phone-data", str(selected)]
    assert arguments[arguments.index("--data-root") + 1] == str(
        command_bundle.root / ".cache/runtime/phone"
    )
    if action == "sudo-keyboard":
        assert child.call_args.kwargs["user"] == 1234


def test_verify_selects_the_same_phone_for_build_inputs_and_authenticated_reconnect(
    command_bundle: CommandBundleContext,
) -> None:
    """An external-root build and its live comparison cannot select different phone inputs."""
    selected = command_bundle.root / "second phone"
    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(
            build_inputs, "target_workspace_snapshot", return_value=command_bundle.snapshot
        ) as capture,
        mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
        mock.patch.object(
            subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as child,
    ):
        runtime.verify_booted("phone", phone_data=selected)

    assert capture.call_args == mock.call("phone", None, build_type="release", phone_data=selected)
    arguments = child.call_args.args[0]
    selector = arguments.index("--phone-data")
    assert arguments[selector : selector + 2] == ["--phone-data", str(selected)]
