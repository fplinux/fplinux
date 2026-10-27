# SPDX-License-Identifier: GPL-2.0-only
"""Run-command generation selection and isolated bundled runtime invocation."""

from __future__ import annotations

import contextlib
import os
import subprocess
import sys
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session, runner

from tests.small.build.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.build.command_fixtures import CommandBundleContext


class RunLifecycleTests:
    """Select one immutable generation before starting any external operation."""

    def test_run_refuses_an_unbuilt_type_before_starting_the_runtime(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """A release bundle cannot satisfy an explicit debug run."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(os, "execv") as execute,
            pytest.raises(SystemExit, match="build phone --build-type debug"),
        ):
            runner.run_target("phone", build_type="debug")
        execute.assert_not_called()

    @pytest.mark.parametrize("loader_trace", [False, True], ids=["default", "loader-trace"])
    def test_run_executes_the_selected_generation_with_checkout_state_roots(
        self, command_bundle: CommandBundleContext, *, loader_trace: bool
    ) -> None:
        """Runtime code and payload come from the selected bundle, with shared local data."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(os, "execv") as execute,
        ):
            runner.run_target("phone", loader_trace=loader_trace)
        expected = [
            sys.executable,
            "-I",
            str(command_bundle.bundle_path / "fplinux.py"),
            "run",
            "--bundle",
            str(command_bundle.bundle_path),
            "--data-root",
            str(command_bundle.root / ".cache/runtime/phone"),
            "--log-root",
            str(command_bundle.root / ".cache/logs/run/phone"),
        ]
        if loader_trace:
            expected.append("--loader-trace")
        execute.assert_called_once_with(sys.executable, expected)

    def test_dump_and_event_paths_are_resolved_from_the_invoking_directory(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Run and explicit preparation retain the caller's input and event destinations."""
        with (
            contextlib.chdir(command_bundle.root),
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(os, "execv") as execute,
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as child,
        ):
            runner.run_target(
                "phone", from_dump=Path("saved/nand.bin"), events=Path("events/load.jsonl")
            )
            runner.prepare_target("phone", replace=True, events=Path("events/load.jsonl"))
        assert execute.call_args.args[1][-4:] == [
            "--from-dump",
            str(command_bundle.root / "saved/nand.bin"),
            "--events",
            str(command_bundle.root / "events/load.jsonl"),
        ]
        arguments = child.call_args.args[0]
        assert arguments[:4] == [
            sys.executable,
            "-I",
            str(command_bundle.bundle_path / "fplinux.py"),
            "prepare",
        ]
        assert arguments[-3:] == [
            "--replace",
            "--events",
            str(command_bundle.root / "events/load.jsonl"),
        ]
        assert child.call_args.kwargs == {"stdin": subprocess.DEVNULL, "check": False}

    def test_preparation_preserves_the_bundled_runtime_failure_status(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Reader failure cannot be reported as successful source preparation."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 130)
            ),
            pytest.raises(SystemExit) as error,
        ):
            runner.prepare_target("phone")
        assert error.value.code == 130

    def test_microsd_context_selection_has_no_fallback(self) -> None:
        """Public boot selection and an explicit profile select one exact context."""
        assert (
            bundle_session.selected_context_profile("nokia-ta1618", profile=None, boot="microsd")
            == "microsd-uboot"
        )
        assert (
            bundle_session.selected_context_profile("nokia-ta1618", profile=None, boot=None)
            is None
        )
        assert (
            bundle_session.selected_context_profile(
                "nokia-ta1618", profile="microsd-uboot", boot=None
            )
            == "microsd-uboot"
        )
        with pytest.raises(SystemExit, match="cannot be used together"):
            bundle_session.selected_context_profile(
                "nokia-ta1618", profile="microsd-uboot", boot="microsd"
            )

    @pytest.mark.parametrize("selection", ["boot", "profile"])
    def test_profile_run_uses_its_generation_and_profile_log_context(
        self, command_bundle: CommandBundleContext, selection: str
    ) -> None:
        """A named profile keeps target data shared while selecting its own image and journal."""
        profile = "microsd-uboot"
        profile_path = command_bundle.create_generation("b" * 64, profile=profile)
        publish_current_bundle(command_bundle.output, "phone", profile_path, profile)
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                targets, "load_target", return_value={"runtime": {"runnable": True}}
            ),
            mock.patch.object(os, "execv") as execute,
        ):
            if selection == "boot":
                runner.run_target("phone", boot="microsd")
            else:
                runner.run_target("phone", profile=profile)
        arguments = execute.call_args.args[1]
        assert arguments[2] == str(profile_path / "fplinux.py")
        assert arguments[5] == str(profile_path)
        assert arguments[7] == str(command_bundle.root / ".cache/runtime/phone")
        assert arguments[9] == str(
            command_bundle.root / ".cache/logs/run/phone/profiles/microsd-uboot"
        )

    def test_stale_build_only_profile_bundle_remains_non_runnable(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Changing source policy cannot authorize an older build-only bundle."""
        profile = "microsd-uboot"
        path = command_bundle.create_generation("c" * 64, profile=profile, runnable=False)
        publish_current_bundle(command_bundle.output, "phone", path, profile)
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                targets, "load_target", return_value={"runtime": {"runnable": True}}
            ),
            mock.patch.object(os, "execv") as execute,
            pytest.raises(SystemExit, match="profile bundle is build-only"),
        ):
            runner.run_target("phone", profile=profile)
        execute.assert_not_called()
