# SPDX-License-Identifier: GPL-2.0-only
"""Run-command generation selection and controlled loader invocation."""

from __future__ import annotations

import contextlib
import os
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session, runner

from tests.small.build.command_fixtures import CommandBundleFixture


class RunLifecycleTests(CommandBundleFixture):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_run_refuses_an_unbuilt_type_before_starting_the_loader(self) -> None:
        """A release bundle cannot satisfy an explicit debug run."""
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch(
                "fplinux_cli.runtime.runner.os.execv",
                side_effect=AssertionError("missing bundle must not run"),
            ),
            self.assertRaisesRegex(SystemExit, "build phone --build-type debug"),
        ):
            runner.run_target("phone", build_type="debug")

    def test_loader_event_path_is_resolved_from_the_invoking_directory(self) -> None:
        """Both runner entry points receive an absolute event destination owned by the caller."""
        runner_path = self.bundle_path / "runner/run.py"
        with (
            contextlib.chdir(self.root),
            mock.patch.object(common, "ROOT", self.root),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
            mock.patch(
                "fplinux_cli.runtime.runner.subprocess.run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as child,
        ):
            runner.run_target("phone", events=Path("events/load.jsonl"))
            runner.run_target_noninteractive("phone", events=Path("events/load.jsonl"))
        expected = [
            os.fsencode(runner_path),
            b"--events",
            os.fsencode(self.root / "events/load.jsonl"),
        ]
        self.assertEqual(execute.call_args.args[1], expected)
        self.assertEqual(child.call_args.args[0], expected)

    def test_run_executes_a_runner_from_the_resolved_generation(self) -> None:
        """Resolve current once and preserve that immutable generation path."""
        runner_path = self.bundle_path / "runner/run.py"
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value={}),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
        ):
            runner.run_target("phone")

        execute.assert_called_once_with(os.fsencode(runner_path), [os.fsencode(runner_path)])

    def test_microsd_context_selection_has_no_fallback(self) -> None:
        """Resolve the Nokia microSD context without making the profile an alias."""
        self.assertEqual(
            bundle_session.selected_context_profile(
                "nokia-ta1618",
                profile=None,
                boot="microsd",
            ),
            "microsd-uboot",
        )
        self.assertIsNone(
            bundle_session.selected_context_profile("nokia-ta1618", profile=None, boot=None)
        )
        self.assertEqual(
            bundle_session.selected_context_profile(
                "nokia-ta1618",
                profile="microsd-uboot",
                boot=None,
            ),
            "microsd-uboot",
        )
        self.assertEqual(
            bundle_session.selected_context_profile(
                "inoi-240-modern-4g", profile=None, boot="microsd"
            ),
            "microsd-uboot",
        )
        with self.assertRaisesRegex(SystemExit, "cannot be used together"):
            bundle_session.selected_context_profile(
                "nokia-ta1618",
                profile="microsd-uboot",
                boot="microsd",
            )

    def test_microsd_context_opens_the_selected_profile_runner(self) -> None:
        """The microSD boot mode runs the microSD profile's generation, not the default one."""
        profile_path = self._create_generation("b" * 64, profile="microsd-uboot")
        publish_current_bundle(self.output, "phone", profile_path, "microsd-uboot")
        runner_path = profile_path / "runner/run.py"

        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                targets,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
        ):
            runner.run_target("phone", boot="microsd")

        execute.assert_called_once_with(os.fsencode(runner_path), [os.fsencode(runner_path)])

    def test_run_profile_executes_only_that_profiles_current_generation(self) -> None:
        """A named run does not fall back to the target's default bundle pointer."""
        profile = "microsd-uboot"
        profile_path = self._create_generation("b" * 64, profile=profile)
        profile_bundle = publish_current_bundle(
            self.output,
            "phone",
            profile_path,
            profile,
        )
        runner_path = profile_bundle.path / "runner/run.py"

        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                targets,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
        ):
            runner.run_target("phone", profile=profile)

        execute.assert_called_once_with(os.fsencode(runner_path), [os.fsencode(runner_path)])

    def test_build_only_profile_is_rejected_before_bundle_or_usb_access(self) -> None:
        """A profile without a complete boot path cannot start the RAM loader."""
        with (
            mock.patch.object(
                targets,
                "load_target",
                return_value={"runtime": {"runnable": False}},
            ),
            mock.patch.object(
                bundle_session,
                "resolve_target_bundle",
                side_effect=AssertionError("build-only profile must not resolve a bundle"),
            ),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
            self.assertRaisesRegex(SystemExit, "profile is build-only"),
        ):
            runner.run_target("phone", profile="microsd-uboot")

        execute.assert_not_called()

    def test_stale_build_only_profile_bundle_remains_non_runnable(self) -> None:
        """Changing source policy cannot authorize an older build-only bundle."""
        profile = "microsd-uboot"
        profile_path = self._create_generation("c" * 64, profile=profile, runnable=False)
        publish_current_bundle(self.output, "phone", profile_path, profile)
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                targets,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ),
            mock.patch("fplinux_cli.runtime.runner.os.execv") as execute,
            self.assertRaisesRegex(SystemExit, "profile bundle is build-only"),
        ):
            runner.run_target("phone", profile=profile)

        execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
