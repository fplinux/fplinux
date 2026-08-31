# SPDX-License-Identifier: GPL-2.0-only
"""Small parser and dispatch tests for target profile selection."""

from __future__ import annotations

import sys
import unittest
from unittest import mock

from fplinux_cli import __main__ as cli


class ProfileParserDispatchTests(unittest.TestCase):
    """Keep profile selection explicit in the in-process parser and dispatcher."""

    def invoke(
        self, *arguments: str
    ) -> tuple[mock.Mock, mock.Mock, mock.Mock, mock.Mock, mock.Mock]:
        """Parse one command without taking a lock or starting external work."""
        check = mock.Mock()
        build = mock.Mock()
        run = mock.Mock()
        package = mock.Mock()
        console = mock.Mock()
        with (
            mock.patch.object(
                cli,
                "discover_targets",
                return_value=("demo", "nokia-ta1618"),
            ),
            mock.patch.object(cli, "check", check),
            mock.patch.object(cli, "build", build),
            mock.patch.object(cli, "run_target", run),
            mock.patch.object(cli, "package_target", package),
            mock.patch.object(cli, "console_target", console),
            mock.patch.object(
                cli,
                "_dispatch_with_cache_lock",
                side_effect=lambda _args, action: action(),
            ),
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
        ):
            cli.main()
        return check, build, run, package, console

    def test_profile_check_without_scopes_runs_only_the_profile_kernel(self) -> None:
        """A profile request never runs unrelated source scopes."""
        check, _build, _run, _package, _console = self.invoke("check", "--profile", "usb-host-lab")

        check.assert_called_once_with(
            ["kernel"],
            profile="usb-host-lab",
            verbose=False,
            no_cache=False,
            jobs=3,
        )

    def test_profile_check_accepts_only_the_explicit_kernel_scope(self) -> None:
        """The selected profile's kernel can be checked without a broad quality gate."""
        check, _build, _run, _package, _console = self.invoke(
            "check", "kernel", "--profile", "usb-host-lab"
        )

        check.assert_called_once_with(
            ["kernel"],
            profile="usb-host-lab",
            verbose=False,
            no_cache=False,
            jobs=3,
        )

    def test_profile_check_forwards_an_explicit_kernel_worker_limit(self) -> None:
        """Keep a named profile on the same bounded kernel-analysis path."""
        check, _build, _run, _package, _console = self.invoke(
            "check",
            "--profile",
            "usb-host-lab",
            "--jobs",
            "2",
        )

        check.assert_called_once_with(
            ["kernel"],
            profile="usb-host-lab",
            verbose=False,
            no_cache=False,
            jobs=2,
        )

    def test_profile_check_rejects_unrelated_scopes(self) -> None:
        """A profile does not silently imply a source or repository check."""
        with self.assertRaisesRegex(SystemExit, "2"):
            self.invoke("check", "source", "--profile", "usb-host-lab")

    def test_build_and_run_forward_the_same_named_profile(self) -> None:
        """Build and RAM run select the same target-owned slot."""
        _check, build, _run, _package, _console = self.invoke(
            "build",
            "demo",
            "--profile",
            "usb-host-lab",
            "--jobs",
            "3",
        )
        build.assert_called_once_with(
            "demo",
            3,
            profile="usb-host-lab",
            verbose=False,
            offline=False,
        )

        _check, _build, run, _package, _console = self.invoke(
            "run", "demo", "--profile", "usb-host-lab"
        )
        run.assert_called_once_with("demo", profile="usb-host-lab", boot=None)

    def test_package_and_console_forward_the_same_named_profile(self) -> None:
        """Qualification archives and reconnects select the profile generation explicitly."""
        _check, _build, _run, package, _console = self.invoke(
            "package",
            "demo",
            "--profile",
            "usb-host-lab",
            "--candidate",
        )
        package.assert_called_once_with(
            "demo",
            profile="usb-host-lab",
            boot=None,
            candidate=True,
        )

        _check, _build, _run, _package, console = self.invoke(
            "console",
            "demo",
            "--profile",
            "usb-host-lab",
            "--exec",
            "id",
        )
        console.assert_called_once_with(
            "demo",
            profile="usb-host-lab",
            keyboard=None,
            exec_command="id",
            upload=None,
            pull=None,
        )

    def test_microsd_boot_selector_is_forwarded_separately_from_profiles(self) -> None:
        """The boot selector and contributor profile choose distinct dispatch inputs."""
        _check, _build, run, _package, _console = self.invoke(
            "run",
            "nokia-ta1618",
            "--boot",
            "microsd",
        )
        run.assert_called_once_with(
            "nokia-ta1618",
            profile=None,
            boot="microsd",
        )

        _check, _build, _run, package, _console = self.invoke(
            "package",
            "nokia-ta1618",
            "--boot",
            "microsd",
            "--candidate",
        )
        package.assert_called_once_with(
            "nokia-ta1618",
            profile=None,
            boot="microsd",
            candidate=True,
        )

    def test_boot_selector_and_contributor_profile_are_mutually_exclusive(self) -> None:
        """One parser invocation selects either a boot mode or a contributor profile."""
        for command in ("run", "package"):
            with (
                self.subTest(command=command),
                self.assertRaisesRegex(SystemExit, "2"),
            ):
                self.invoke(
                    command,
                    "nokia-ta1618",
                    "--boot",
                    "microsd",
                    "--profile",
                    "microsd-uboot",
                )


if __name__ == "__main__":
    unittest.main()
