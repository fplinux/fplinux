# SPDX-License-Identifier: GPL-2.0-only
"""Console and keyboard selection of an existing authenticated bundled session."""

from __future__ import annotations

import os
import pwd
import subprocess
import sys
from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.cli.runtime as runtime_commands
import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.manifests import targets

from tests.small.build.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.build.command_fixtures import CommandBundleContext


def keyboard_config() -> dict[str, object]:
    """Declare the synthetic phone's USB keyboard interface."""
    return {
        "runtime": {
            "usb": {
                "linux_gadget": {
                    "vendor_id": 0x0525,
                    "product_id": 0xA4A6,
                    "wait_seconds": 10,
                    "keyboard_interface": 1,
                }
            }
        }
    }


class ConsoleLifecycleTests:
    """Preserve remote actions while letting the selected runtime own authentication."""

    @pytest.mark.parametrize(
        ("exec_command", "upload", "pull", "action"),
        [
            (None, None, None, []),
            ("id", None, None, ["--exec", "id"]),
            (
                None,
                ["local.bin", "/phone/remote.bin"],
                None,
                ["--upload", "local.bin", "/phone/remote.bin"],
            ),
            (
                None,
                None,
                ["/phone/remote.bin", "local.bin"],
                ["--pull", "/phone/remote.bin", "local.bin"],
            ),
        ],
        ids=["shell", "exec", "upload", "pull"],
    )
    def test_console_routes_actions_to_the_selected_runtime_without_booting(
        self,
        command_bundle: CommandBundleContext,
        exec_command: str | None,
        upload: list[str] | None,
        pull: list[str] | None,
        action: list[str],
    ) -> None:
        """Every console mode requires reconnect and receives the shared personal state root."""
        profile = "microsd-uboot"
        path = command_bundle.create_generation("b" * 64, profile=profile)
        publish_current_bundle(command_bundle.output, "phone", path, profile)
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as child,
        ):
            runtime_commands.console_target(
                "phone",
                profile=profile,
                keyboard=None,
                exec_command=exec_command,
                upload=upload,
                pull=pull,
            )
        assert child.call_args.args[0] == [
            sys.executable,
            "-I",
            str(path / "fplinux.py"),
            "run",
            "--bundle",
            str(path),
            "--data-root",
            str(command_bundle.root / ".cache/runtime/phone"),
            "--log-root",
            str(command_bundle.root / ".cache/logs/run/phone/profiles/microsd-uboot"),
            "--reconnect",
            *action,
        ]
        assert child.call_args.kwargs == {"check": False}

    def test_console_preserves_remote_failure_status(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """A remote command failure remains the public console's exit status."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 7)),
            pytest.raises(SystemExit) as error,
        ):
            runtime_commands.console_target(
                "phone", keyboard=None, exec_command="false", upload=None, pull=None
            )
        assert error.value.code == 7

    @pytest.mark.parametrize("status", [1, 0], ids=["rejected-session", "authenticated"])
    def test_keyboard_forwarding_requires_the_selected_runtime_to_authenticate(
        self, command_bundle: CommandBundleContext, status: int
    ) -> None:
        """The host keyboard bridge cannot start after session, data or kernel rejection."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=keyboard_config()),
            mock.patch.object(os, "geteuid", return_value=1000),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], status)
            ) as authenticate,
            mock.patch.object(os, "execv") as execute,
        ):
            if status:
                with pytest.raises(SystemExit):
                    runtime_commands.console_target(
                        "phone",
                        keyboard="/dev/input/event1",
                        exec_command=None,
                        upload=None,
                        pull=None,
                    )
            else:
                runtime_commands.console_target(
                    "phone",
                    keyboard="/dev/input/event1",
                    exec_command=None,
                    upload=None,
                    pull=None,
                )
        assert authenticate.call_args.args[0][2] == str(command_bundle.bundle_path / "fplinux.py")
        assert authenticate.call_args.args[0][-3:] == ["--reconnect", "--exec", "true"]
        if status:
            execute.assert_not_called()
        else:
            client = command_bundle.bundle_path / "host/fplinux-usb-keyboard"
            execute.assert_called_once_with(
                client,
                [
                    str(client),
                    "--vid",
                    "0525",
                    "--pid",
                    "a4a6",
                    "--wait",
                    "10",
                    "--interface",
                    "1",
                    "--keyboard",
                    "/dev/input/event1",
                ],
            )

    @pytest.mark.parametrize("status", [1, 0], ids=["verification-failed", "verified"])
    def test_sudo_keyboard_authenticates_the_same_bundle_as_the_invoking_user(
        self, command_bundle: CommandBundleContext, status: int
    ) -> None:
        """Privileged forwarding uses the owner's selected data without a second source launch."""
        profile = "microsd-uboot"
        path = command_bundle.create_generation("d" * 64, profile=profile, build_type="debug")
        publish_current_bundle(command_bundle.output, "phone", path, profile, build_type="debug")
        uid, gid = 1234, 2345
        account = pwd.struct_passwd(
            ("caller", "x", uid, gid, "", str(command_bundle.root / "caller-home"), "/bin/sh")
        )
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=keyboard_config()),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"SUDO_UID": str(uid), "SUDO_USER": "caller"}),
            mock.patch.object(pwd, "getpwuid", return_value=account),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], status),
            ) as authenticate,
            mock.patch.object(os, "execv") as execute,
        ):
            if status:
                with pytest.raises(SystemExit):
                    runtime_commands.console_target(
                        "phone",
                        profile=profile,
                        build_type="debug",
                        keyboard="/dev/input/event1",
                        exec_command=None,
                        upload=None,
                        pull=None,
                    )
            else:
                runtime_commands.console_target(
                    "phone",
                    profile=profile,
                    build_type="debug",
                    keyboard="/dev/input/event1",
                    exec_command=None,
                    upload=None,
                    pull=None,
                )
        arguments = authenticate.call_args.args[0]
        assert arguments[:4] == [sys.executable, "-I", str(path / "fplinux.py"), "run"]
        assert arguments[7] == str(command_bundle.root / ".cache/runtime/phone")
        assert arguments[-3:] == ["--reconnect", "--exec", "true"]
        options = authenticate.call_args.kwargs
        assert (options["user"], options["group"], options["extra_groups"]) == (uid, gid, ())
        assert options["env"]["HOME"] == account.pw_dir
        assert options["env"]["USER"] == "caller"
        if status:
            execute.assert_not_called()
        else:
            assert execute.call_args.args[0] == path / "host/fplinux-usb-keyboard"
