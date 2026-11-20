# SPDX-License-Identifier: GPL-2.0-only
"""Console and keyboard selection of an existing authenticated bundled session."""

from __future__ import annotations

import json
import os
import pwd
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.cli.runtime as runtime_commands
import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session
from fplinux_cli.runtime import keyboard as keyboard_runtime
from fplinux_cli.runtime import live as live_runtime
from fplinux_cli.runtime.data_paths import checkout_runtime_root
from fplinux_cli.runtime.ssh import transport as ssh_transport

from tests.small.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from tests.small.command_fixtures import CommandBundleContext


def save_connection(root: Path, *, target: str = "phone") -> None:
    session = create_ready_session(root, target=target)
    current = root / "current" / f"{target}.json"
    current.parent.mkdir(mode=0o700)
    current.write_text(json.dumps(session), encoding="utf-8")
    current.chmod(0o600)


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

    @pytest.mark.parametrize("target", [None, "phone"], ids=["discover", "explicit"])
    def test_console_selects_only_this_installations_live_session(
        self, command_bundle: CommandBundleContext, target: str | None
    ) -> None:
        """Discovery is installation-scoped and an explicit target excludes other phones."""
        scope = checkout_runtime_root(command_bundle.cache, command_bundle.root)
        save_connection(scope / "phone/session", target="phone")
        other = checkout_runtime_root(command_bundle.cache, command_bundle.root / "other-checkout")
        save_connection(other / "other/session", target="other")
        if target is not None:
            save_connection(scope / "other/session", target="other")
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(ssh_transport, "usb_present", return_value=True),
            mock.patch.object(live_runtime, "console_connection", return_value=0) as live,
        ):
            runtime_commands.console_target(
                target,
                keyboard=None,
                exec_command="id",
                upload=None,
                pull=None,
                allow_other_build=True,
            )

        assert live.call_args.args[0][1]["target"] == "phone"

    @pytest.mark.parametrize(
        ("exec_command", "upload", "pull"),
        [
            (None, None, None),
            ("id", None, None),
            (
                None,
                ["local.bin", "/phone/remote.bin"],
                None,
            ),
            (
                None,
                None,
                ["/phone/remote.bin", "local.bin"],
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
    ) -> None:
        """Diagnostic actions use the selected live phone without resolving a build."""
        profile = "microsd-uboot"
        session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
        save_connection(session_root / "session")
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(ssh_transport, "usb_present", return_value=True),
            mock.patch.object(live_runtime, "console_connection", return_value=0) as live,
            mock.patch.object(runtime_commands, "admit_bundle", side_effect=AssertionError),
            mock.patch.object(
                bundle_session,
                "resolve_target_bundle",
                side_effect=AssertionError,
            ),
        ):
            runtime_commands.console_target(
                "phone",
                profile=profile,
                keyboard=None,
                exec_command=exec_command,
                upload=upload,
                pull=pull,
                allow_other_build=True,
            )
        connection, data, logs = live.call_args.args
        assert connection[1]["target"] == "phone"
        assert data == session_root
        assert logs == command_bundle.cache / "logs/run/phone/profiles/microsd-uboot"
        options = live.call_args.kwargs
        assert options["exec_command"] == exec_command
        if upload is not None or pull is not None:
            transfer = options["transfer"]
            assert transfer.direction == ("upload" if upload is not None else "pull")
            assert transfer.local == Path("local.bin")
            assert transfer.remote == "/phone/remote.bin"
        else:
            assert options["transfer"] is None
        assert options["admitted"] is None
        assert options["revalidate"] is None

    def test_console_preserves_remote_failure_status(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """A remote command failure remains the public console's exit status."""
        session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
        save_connection(session_root / "session")
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(ssh_transport, "usb_present", return_value=True),
            mock.patch.object(live_runtime, "console_connection", return_value=7),
            pytest.raises(SystemExit) as error,
        ):
            runtime_commands.console_target(
                "phone",
                keyboard=None,
                exec_command="false",
                upload=None,
                pull=None,
                allow_other_build=True,
            )
        assert error.value.code == 7

    @pytest.mark.parametrize("status", [1, 0], ids=["rejected-session", "authenticated"])
    @pytest.mark.parametrize("allow_other_build", [False, True], ids=["strict", "diagnostic"])
    def test_keyboard_forwarding_requires_the_selected_runtime_to_authenticate(
        self, command_bundle: CommandBundleContext, status: int, *, allow_other_build: bool
    ) -> None:
        """The host keyboard bridge cannot start after session, data or kernel rejection."""
        if allow_other_build:
            command_bundle.clear_current_bundle()
            slot = command_bundle.cache / "host-tools/fplinux-usb-keyboard"
            slot.mkdir(parents=True)
            client = slot / "binary"
            client.write_bytes(b"keyboard client\n")
            client.chmod(0o755)
            (slot / "receipt.json").write_text(
                '{"recipe":"' + "a" * 64 + '","sha256":'
                '"cb92dc893e9b46fb906d2d019ab26b00c9a7b8feed1270b178c3773f68553739"}',
                encoding="utf-8",
            )
        else:
            client = command_bundle.bundle_path / "host/fplinux-usb-keyboard"
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
                        allow_other_build=allow_other_build,
                    )
            else:
                runtime_commands.console_target(
                    "phone",
                    keyboard="/dev/input/event1",
                    exec_command=None,
                    upload=None,
                    pull=None,
                    allow_other_build=allow_other_build,
                )
        assert authenticate.call_args.args[0] == [
            str(command_bundle.root / "fplinux"),
            "console",
            "phone",
            *(["--allow-other-build"] if allow_other_build else ["--build-type", "release"]),
            "--exec",
            "true",
        ]
        if status:
            execute.assert_not_called()
        else:
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

    def test_diagnostic_keyboard_reports_missing_prepared_client(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Diagnostic mode does not rebuild or fall back to a current bundle."""
        command_bundle.clear_current_bundle()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=keyboard_config()),
            mock.patch.object(os, "geteuid", return_value=1000),
            mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0)),
            mock.patch.object(os, "execv") as execute,
            pytest.raises(SystemExit, match="no prepared USB keyboard client"),
        ):
            runtime_commands.console_target(
                "phone",
                keyboard="/dev/input/event1",
                exec_command=None,
                upload=None,
                pull=None,
                allow_other_build=True,
            )

        execute.assert_not_called()

    @pytest.mark.parametrize("status", [1, 0], ids=["verification-failed", "verified"])
    def test_sudo_keyboard_authenticates_the_same_bundle_as_the_invoking_user(
        self, command_bundle: CommandBundleContext, status: int
    ) -> None:
        """Privileged forwarding verifies current sources as the invoking user."""
        profile = "microsd-uboot"
        path = command_bundle.create_generation("d" * 64, profile=profile, build_type="debug")
        publish_current_bundle(command_bundle.output, "phone", path, profile, build_type="debug")
        uid, gid = 1234, 2345
        account = pwd.struct_passwd(
            ("caller", "x", uid, gid, "", str(command_bundle.root / "caller-home"), "/bin/sh")
        )
        phone_data = command_bundle.root / "another phone"
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
                        phone_data=phone_data,
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
                    phone_data=phone_data,
                    keyboard="/dev/input/event1",
                    exec_command=None,
                    upload=None,
                    pull=None,
                )
        arguments = authenticate.call_args.args[0]
        assert arguments == [
            str(command_bundle.root / "fplinux"),
            "console",
            "phone",
            "--build-type",
            "debug",
            "--profile",
            "microsd-uboot",
            "--phone-data",
            str(phone_data),
            "--exec",
            "true",
        ]
        options = authenticate.call_args.kwargs
        assert (options["user"], options["group"], options["extra_groups"]) == (uid, gid, ())
        assert options["env"]["HOME"] == account.pw_dir
        assert options["env"]["USER"] == "caller"
        if status:
            execute.assert_not_called()
        else:
            assert execute.call_args.args[0] == path / "host/fplinux-usb-keyboard"

    @pytest.mark.parametrize(
        ("sudo_uid", "sudo_user", "message"),
        [
            ("invalid", "caller", "valid SUDO_UID"),
            ("1234", "other", "matching invoking user"),
        ],
    )
    def test_sudo_keyboard_rejects_invalid_invoking_user(
        self,
        command_bundle: CommandBundleContext,
        sudo_uid: str,
        sudo_user: str,
        message: str,
    ) -> None:
        """An unverified sudo identity cannot authenticate or start the bridge."""
        account = pwd.struct_passwd(("caller", "x", 1234, 2345, "", "/home/caller", "/bin/sh"))
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=keyboard_config()),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"SUDO_UID": sudo_uid, "SUDO_USER": sudo_user}),
            mock.patch.object(pwd, "getpwuid", return_value=account),
            mock.patch.object(subprocess, "run") as authenticate,
            mock.patch.object(os, "execv") as execute,
            pytest.raises(SystemExit, match=message),
        ):
            runtime_commands.console_target(
                "phone", keyboard="/dev/input/event1", exec_command=None, upload=None, pull=None
            )

        authenticate.assert_not_called()
        execute.assert_not_called()

    def test_sudo_keyboard_selects_a_live_target_as_the_invoking_user(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Automatic selection does not open the owner's session files as root."""
        account = pwd.struct_passwd(("caller", "x", 1234, 2345, "", "/home/caller", "/bin/sh"))
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"SUDO_UID": "1234", "SUDO_USER": "caller"}),
            mock.patch.object(pwd, "getpwuid", return_value=account),
            mock.patch.object(
                runtime_commands, "_checkout_connection", side_effect=AssertionError("root read")
            ),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "phone\n")
            ) as child,
        ):
            selected = keyboard_runtime.resolve_keyboard_target(None)

        assert selected == "phone"
        arguments = child.call_args.args[0]
        assert arguments[1:3] == ["-I", "-c"]
        assert arguments[-1] == str(command_bundle.root)
        options = child.call_args.kwargs
        assert (options["user"], options["group"], options["extra_groups"]) == (1234, 2345, ())
        assert options["env"]["HOME"] == "/home/caller"
        assert options["stdout"] == subprocess.PIPE

    def test_sudo_keyboard_rejects_invalid_selected_target(self) -> None:
        """Unexpected subprocess output cannot be used as a target or filesystem path."""
        account = pwd.struct_passwd(("caller", "x", 1234, 2345, "", "/home/caller", "/bin/sh"))
        with (
            mock.patch.object(os, "geteuid", return_value=0),
            mock.patch.dict(os.environ, {"SUDO_UID": "1234", "SUDO_USER": "caller"}),
            mock.patch.object(pwd, "getpwuid", return_value=account),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "../phone\n")
            ),
            pytest.raises(SystemExit, match="valid keyboard target"),
        ):
            keyboard_runtime.resolve_keyboard_target(None)
