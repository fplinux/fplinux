# SPDX-License-Identifier: GPL-2.0-only
"""Console-command session selection and controlled keyboard forwarding."""

from __future__ import annotations

import contextlib
import os
import pwd
import subprocess
import unittest
from pathlib import Path
from unittest import mock

import fplinux_cli.cli.runtime as runtime_commands
import fplinux_cli.runtime.keyboard as keyboard_runtime
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session, ssh_transport

from tests.small.build.command_fixtures import CommandBundleFixture


class ConsoleLifecycleTests(CommandBundleFixture):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_console_uses_ssh_for_commands_and_keyboard_tool_for_evdev(self) -> None:
        """Route commands through SSH and evdev through the keyboard client."""
        target_config = {
            "runtime": {
                "usb": {
                    "linux_gadget": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                        "keyboard_interface": 1,
                    },
                },
            },
        }
        result = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        ssh = mock.Mock(run_remote=mock.Mock(return_value=result))
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=target_config),
            mock.patch.object(runtime_commands, "_current_ssh_session", return_value=(ssh, {})),
            mock.patch.object(keyboard_runtime, "_current_ssh_session", return_value=(ssh, {})),
        ):
            runtime_commands.console_target(
                "phone",
                keyboard=None,
                exec_command="id",
                upload=None,
                pull=None,
            )
        ssh.run_remote.assert_called_once_with({}, "id")

        client = self.bundle_path / "host/fplinux-usb-keyboard"
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=target_config),
            mock.patch.object(runtime_commands, "_current_ssh_session", return_value=(ssh, {})),
            mock.patch.object(keyboard_runtime, "_current_ssh_session", return_value=(ssh, {})),
            mock.patch("fplinux_cli.runtime.keyboard.os.geteuid", return_value=1000),
            mock.patch("fplinux_cli.runtime.keyboard.os.execv") as execute,
        ):
            runtime_commands.console_target(
                "phone",
                keyboard="UP",
                exec_command=None,
                upload=None,
                pull=None,
            )
        execute.assert_called_once_with(
            client,
            [
                str(client),
                "--vid",
                "1782",
                "--pid",
                "4d00",
                "--wait",
                "10",
                "--interface",
                "1",
                "--keyboard",
                "UP",
            ],
        )

    def test_keyboard_forwarding_requires_the_selected_kernel_identity(self) -> None:
        """A session reporting another kernel identity cannot receive host keyboard input."""
        target_config = {
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
        for matches in (False, True):
            with self.subTest(matches=matches):
                # The standalone SSH boundary supplies a session; its identity parser is real.
                ssh = mock.Mock()
                ssh.load_bundle_context.return_value = (
                    {"target": "phone"},
                    {"bundle_generation": self.bundle.generation},
                )
                ssh.load_current_session.return_value = {}
                ssh.reacquire_bound_session.return_value = {}
                ssh.require_device_identity.side_effect = ssh_transport.require_device_identity
                suffix = "9" * 16 if matches else "8" * 16
                response = subprocess.CompletedProcess([], 0, f"6.18.42-fplinux-{suffix}\n", "")
                with (
                    mock.patch.object(common, "ROOT", self.root),
                    mock.patch.object(targets, "load_target", return_value=target_config),
                    mock.patch("fplinux_cli.runtime.keyboard.os.geteuid", return_value=1000),
                    mock.patch.object(bundle_session, "_load_bundle_ssh_helper", return_value=ssh),
                    mock.patch.object(ssh_transport, "run_remote", return_value=response),
                    mock.patch("fplinux_cli.runtime.keyboard.os.execv") as execute,
                    contextlib.ExitStack() as stack,
                ):
                    if not matches:
                        stack.enter_context(
                            self.assertRaisesRegex(SystemExit, "different kernel identity")
                        )
                    runtime_commands.console_target(
                        "phone",
                        keyboard="/dev/input/event1",
                        exec_command=None,
                        upload=None,
                        pull=None,
                    )
                if matches:
                    self.assertEqual(
                        execute.call_args.args[0], self.bundle_path / "host/fplinux-usb-keyboard"
                    )
                    self.assertEqual(
                        execute.call_args.args[1][-2:], ["--keyboard", "/dev/input/event1"]
                    )
                else:
                    execute.assert_not_called()

    def test_sudo_keyboard_verifies_as_the_invoking_user_before_privileged_forwarding(
        self,
    ) -> None:
        """The documented sudo command verifies the caller's session and stops on failure."""
        profile = "microsd-uboot"
        debug = self._create_generation("d" * 64, profile=profile, build_type="debug")
        publish_current_bundle(self.output, "phone", debug, profile, build_type="debug")
        target_config = {
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
        uid, gid = 1234, 2345
        caller_runtime = Path("/run/user/1234")
        root_runtime = self.root / "root-runtime"
        root_runtime.mkdir()
        account = pwd.struct_passwd(
            ("caller", "x", uid, gid, "", str(self.root / "caller-home"), "/bin/sh")
        )
        lstat = Path.lstat

        def runtime_metadata(path: Path) -> os.stat_result:
            if path in (caller_runtime, root_runtime):
                values = list(lstat(root_runtime))
                values[4] = uid if path == caller_runtime else 0
                return os.stat_result(values)
            return lstat(path)

        for status in (1, 0):
            with self.subTest(status=status):
                with (
                    mock.patch.object(common, "ROOT", self.root),
                    mock.patch.object(targets, "load_target", return_value=target_config),
                    mock.patch("fplinux_cli.runtime.keyboard.os.geteuid", return_value=0),
                    mock.patch.dict(
                        os.environ,
                        {
                            "SUDO_UID": str(uid),
                            "SUDO_USER": "caller",
                            "XDG_RUNTIME_DIR": str(root_runtime),
                            "HOME": "/root",
                        },
                    ),
                    mock.patch("fplinux_cli.runtime.keyboard.pwd.getpwuid", return_value=account),
                    mock.patch.object(Path, "lstat", runtime_metadata),
                    mock.patch(
                        "fplinux_cli.runtime.keyboard.subprocess.run",
                        return_value=subprocess.CompletedProcess([], status),
                    ) as verify,
                    mock.patch("fplinux_cli.runtime.keyboard.os.execv") as execute,
                    contextlib.ExitStack() as stack,
                ):
                    if status:
                        stack.enter_context(self.assertRaises(SystemExit))
                    runtime_commands.console_target(
                        "phone",
                        profile=profile,
                        build_type="debug",
                        keyboard="/dev/input/event1",
                        exec_command=None,
                        upload=None,
                        pull=None,
                    )
                command = verify.call_args.args[0]
                self.assertEqual(command[:3], [str(self.root / "fplinux"), "console", "phone"])
                # The public parser accepts these options in any order; compare flag/value pairs.
                self.assertCountEqual(
                    zip(command[3::2], command[4::2], strict=True),
                    [("--build-type", "debug"), ("--exec", "true"), ("--profile", profile)],
                )
                options = verify.call_args.kwargs
                self.assertEqual(
                    (options["user"], options["group"], options["extra_groups"]), (uid, gid, ())
                )
                self.assertEqual(options["env"]["HOME"], account.pw_dir)
                self.assertEqual(options["env"]["XDG_RUNTIME_DIR"], str(caller_runtime))
                if status:
                    execute.assert_not_called()
                else:
                    self.assertEqual(
                        execute.call_args.args[0], debug / "host/fplinux-usb-keyboard"
                    )

    def test_console_profile_reconnects_only_through_its_selected_generation(self) -> None:
        """A profile RAM session is never compared with the target's default bundle."""
        profile = "microsd-uboot"
        profile_path = self._create_generation("b" * 64, profile=profile)
        profile_bundle = publish_current_bundle(
            self.output,
            "phone",
            profile_path,
            profile,
        )
        target_config = {
            "runtime": {
                "usb": {
                    "linux_gadget": {
                        "vendor_id": 0x0525,
                        "product_id": 0xA4A6,
                        "wait_seconds": 60,
                        "keyboard_interface": 1,
                    },
                },
            },
        }
        result = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        ssh = mock.Mock(run_remote=mock.Mock(return_value=result))
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=target_config) as load_target,
            mock.patch.object(
                runtime_commands,
                "_current_ssh_session",
                return_value=(ssh, {}),
            ) as current_session,
        ):
            runtime_commands.console_target(
                "phone",
                profile=profile,
                keyboard=None,
                exec_command="id",
                upload=None,
                pull=None,
            )

        load_target.assert_called_once_with("phone", profile, build_type="release")
        selected_bundle, selected_manifest, selected_target = current_session.call_args.args
        self.assertEqual(selected_bundle, profile_bundle)
        self.assertEqual(selected_manifest["profile"], profile)
        self.assertEqual(selected_target, "phone")
        ssh.run_remote.assert_called_once_with({}, "id")


if __name__ == "__main__":
    unittest.main()
