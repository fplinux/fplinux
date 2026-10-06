# SPDX-License-Identifier: GPL-2.0-only
"""Verify-command generation selection and controlled session reconnection."""

from __future__ import annotations

import contextlib
import io
import json
import unittest
from unittest import mock

import fplinux_cli.cli.runtime as runtime_commands
import fplinux_cli.workspace.build_inputs as workspace_inputs
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.environment import images
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session

from tests.small.build.command_fixtures import CommandBundleFixture


class VerifyLifecycleTests(CommandBundleFixture):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_verify_reports_no_success_when_reconnect_fails(self) -> None:
        """A failed reconnect propagates its error without printing the success line."""
        stdout = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(
                images,
                "container_image_recipe_digest",
                return_value="e" * 64,
            ),
            mock.patch.object(
                runtime_commands,
                "_current_ssh_session",
                side_effect=SystemExit(
                    "fplinux ssh: cannot read the running kernel identity (exit 7)"
                ),
            ),
            contextlib.redirect_stdout(stdout),
            self.assertRaisesRegex(SystemExit, r"running kernel identity \(exit 7\)"),
        ):
            runtime_commands.verify_booted("phone")

        self.assertEqual(stdout.getvalue(), "")

    def test_reconnect_does_not_set_the_phone_clock(self) -> None:
        """Reacquiring a running session never calls the bundle helper's clock sync."""
        manifest = json.loads(self.bundle.manifest_bytes)
        # The bundle's SSH helper is the phone boundary; only its clock call is observed.
        ssh = mock.Mock()
        ssh.load_bundle_context.return_value = (
            {"target": "phone"},
            {"bundle_generation": self.bundle.generation},
        )

        with mock.patch.object(bundle_session, "_load_bundle_ssh_helper", return_value=ssh):
            bundle_session._current_ssh_session(self.bundle, manifest, "phone")  # noqa: SLF001

        # Only a fresh run sets the phone clock; reconnecting leaves it running.
        ssh.sync_clock.assert_not_called()

    def test_verify_resolves_the_selected_microsd_generation(self) -> None:
        """Verification checks the selected bundle identity without falling back to RAM."""
        profile = "microsd-uboot"
        path = self._create_generation("b" * 64, profile=profile)
        selected = publish_current_bundle(self.output, "phone", path, profile)
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                workspace_inputs, "target_workspace_snapshot", return_value=self.snapshot
            ) as snapshot,
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                runtime_commands, "_current_ssh_session", return_value=(mock.Mock(), {})
            ) as session,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            runtime_commands.verify_booted("phone", profile=profile)
        snapshot.assert_called_once_with("phone", profile, build_type="release")
        self.assertEqual(session.call_args.args[0], selected)
        self.assertEqual(session.call_args.args[1]["profile"], profile)

    def test_verify_reports_the_manifest_identity_after_reconnect_accepts(self) -> None:
        """Report the selected device identity after the reconnect boundary accepts it."""
        target_config: dict[str, object] = {}
        stdout = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=target_config),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                runtime_commands,
                "_current_ssh_session",
                return_value=(mock.Mock(), {}),
            ) as current_session,
            contextlib.redirect_stdout(stdout),
        ):
            runtime_commands.verify_booted("phone")

        self.assertEqual(
            stdout.getvalue(),
            "verify: the phone runs the current release build (9999999999999999)\n",
        )
        current_session.assert_called_once_with(self.bundle, mock.ANY, "phone")


if __name__ == "__main__":
    unittest.main()
