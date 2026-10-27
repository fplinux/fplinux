# SPDX-License-Identifier: GPL-2.0-only
"""Verify-command generation selection and controlled session reconnection."""

from __future__ import annotations

import contextlib
import io
import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.cli.runtime as runtime_commands
import fplinux_cli.workspace.build_inputs as workspace_inputs
import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.environment import definition
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.small.build.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.build.command_fixtures import CommandBundleContext


class VerifyLifecycleTests:
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_verify_accepts_the_clean_build_while_local_preparation_exists(
        self,
        command_bundle: CommandBundleContext,
    ) -> None:
        """An ordinary snapshot containing device data cannot stale the selected clean build."""
        ordinary = WorkspaceSnapshot((), "d" * 64)
        path = command_bundle.create_generation("0" * 64)
        publish_current_bundle(command_bundle.output, "phone", path)
        stdout = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                side_effect=[ordinary, command_bundle.snapshot],
            ) as snapshot,
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as reconnect,
            contextlib.redirect_stdout(stdout),
        ):
            runtime_commands.verify_booted("phone")

        assert snapshot.call_args_list == [
            mock.call(
                "phone",
                None,
                build_type="release",
                phone_data=None,
                selected_data_root=command_bundle.cache,
            ),
            mock.call(
                "phone",
                None,
                build_type="release",
                include_device_data=False,
                phone_data=None,
                selected_data_root=command_bundle.cache,
            ),
        ]
        assert reconnect.call_args.args[0][2] == str(path / "fplinux.py")
        assert reconnect.call_args.args[0][-3:] == ["--reconnect", "--exec", "true"]
        assert stdout.getvalue() == (
            "verify: the phone runs the current release build (9999999999999999)\n"
        )

    def test_verify_rejects_when_neither_supported_snapshot_matches_the_bundle(
        self,
        command_bundle: CommandBundleContext,
    ) -> None:
        """A clean fallback cannot admit changed sources or another build's device-data choice."""
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                side_effect=[
                    WorkspaceSnapshot((), "d" * 64),
                    WorkspaceSnapshot((), "e" * 64),
                ],
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(subprocess, "run") as reconnect,
            pytest.raises(SystemExit, match="build output is stale"),
        ):
            runtime_commands.verify_booted("phone")

        reconnect.assert_not_called()

    def test_verify_reports_no_success_when_reconnect_fails(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """A failed reconnect propagates its error without printing the success line."""
        stdout = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(
                definition,
                "environment_recipe_digest",
                return_value="e" * 64,
            ),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 7),
            ),
            contextlib.redirect_stdout(stdout),
            pytest.raises(SystemExit) as error,
        ):
            runtime_commands.verify_booted("phone")

        assert (stdout.getvalue()) == ("")
        assert error.value.code == 7

    def test_verify_resolves_the_selected_microsd_generation(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Verification checks the selected bundle identity without falling back to RAM."""
        profile = "microsd-uboot"
        path = command_bundle.create_generation("b" * 64, profile=profile)
        publish_current_bundle(command_bundle.output, "phone", path, profile)
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                workspace_inputs, "target_workspace_snapshot", return_value=command_bundle.snapshot
            ) as snapshot,
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as session,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            runtime_commands.verify_booted("phone", profile=profile)
        snapshot.assert_called_once_with(
            "phone",
            profile,
            build_type="release",
            phone_data=None,
            selected_data_root=command_bundle.cache,
        )
        arguments = session.call_args.args[0]
        assert arguments[2] == str(path / "fplinux.py")
        assert arguments[9] == str(command_bundle.cache / "logs/run/phone/profiles/microsd-uboot")

    def test_verify_reports_the_manifest_identity_after_reconnect_accepts(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Report the selected device identity after the reconnect boundary accepts it."""
        stdout = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0),
            ) as current_session,
            contextlib.redirect_stdout(stdout),
        ):
            runtime_commands.verify_booted("phone")

        assert (stdout.getvalue()) == (
            "verify: the phone runs the current release build (9999999999999999)\n"
        )
        assert current_session.call_args.args[0][-3:] == ["--reconnect", "--exec", "true"]
