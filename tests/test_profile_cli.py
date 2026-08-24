# SPDX-License-Identifier: GPL-2.0-only
"""Public command parsing for isolated target profiles."""

from __future__ import annotations

import sys
import unittest
from unittest import mock

from fplinux_cli import __main__ as cli


class ProfileCommandLineTests(unittest.TestCase):
    """Keep profile selection on the three supported public commands only."""

    def invoke(self, *arguments: str) -> tuple[mock.Mock, mock.Mock, mock.Mock]:
        """Parse one command without taking a lock or starting external work."""
        check = mock.Mock()
        build = mock.Mock()
        run = mock.Mock()
        with (
            mock.patch.object(cli, "discover_targets", return_value=("demo",)),
            mock.patch.object(cli, "check", check),
            mock.patch.object(cli, "build", build),
            mock.patch.object(cli, "run_target", run),
            mock.patch.object(
                cli,
                "_dispatch_with_cache_lock",
                side_effect=lambda _args, action: action(),
            ),
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
        ):
            cli.main()
        return check, build, run

    def test_profile_check_without_scopes_runs_only_the_profile_kernel(self) -> None:
        """A profile request never runs unrelated source scopes."""
        check, _build, _run = self.invoke("check", "--profile", "usb-host-lab")

        check.assert_called_once_with(
            ["kernel"],
            profile="usb-host-lab",
            verbose=False,
            no_cache=False,
        )

    def test_profile_check_accepts_only_the_explicit_kernel_scope(self) -> None:
        """The selected profile's kernel can be checked without a broad quality gate."""
        check, _build, _run = self.invoke("check", "kernel", "--profile", "usb-host-lab")

        check.assert_called_once_with(
            ["kernel"],
            profile="usb-host-lab",
            verbose=False,
            no_cache=False,
        )

    def test_profile_check_rejects_unrelated_scopes(self) -> None:
        """A profile does not silently imply a source or repository check."""
        with self.assertRaisesRegex(SystemExit, "2"):
            self.invoke("check", "source", "--profile", "usb-host-lab")

    def test_build_and_run_forward_the_same_named_profile(self) -> None:
        """Build and RAM run select the same target-owned slot."""
        _check, build, _run = self.invoke(
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

        _check, _build, run = self.invoke("run", "demo", "--profile", "usb-host-lab")
        run.assert_called_once_with("demo", profile="usb-host-lab")

    def test_package_does_not_accept_a_profile(self) -> None:
        """A profile remains a lab build and cannot enter package selection."""
        with self.assertRaisesRegex(SystemExit, "2"):
            self.invoke("package", "demo", "--profile", "usb-host-lab")


if __name__ == "__main__":
    unittest.main()
