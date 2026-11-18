# SPDX-License-Identifier: GPL-2.0-only
"""Current-source checks for checkout console and verification."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.cli import runtime
from fplinux_cli.environment import definition
from fplinux_cli.runtime import live as live_runtime
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.data_paths import checkout_runtime_root
from fplinux_cli.runtime.ssh import transport as ssh_transport
from fplinux_cli.workspace import build_inputs
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.small.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.command_fixtures import CommandBundleContext


def save_connection(root: Path) -> None:
    session = create_ready_session(root)
    current = root / "current/phone.json"
    current.parent.mkdir(mode=0o700)
    current.write_text(json.dumps(session), encoding="utf-8")
    current.chmod(0o600)


@pytest.mark.parametrize("operation", ["console", "verify"])
@pytest.mark.parametrize("fresh", [True, False], ids=["current", "changed-source"])
def test_strict_commands_check_current_source_before_action(
    command_bundle: CommandBundleContext, operation: str, *, fresh: bool
) -> None:
    """Both entry points reject a changed snapshot before the remote action."""
    session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
    save_connection(session_root / "session")
    snapshot = command_bundle.snapshot if fresh else WorkspaceSnapshot((), "d" * 64)
    admitted = ValidatedBundle(
        command_bundle.bundle_path, {"target": "phone"}, "a" * 64, "9" * 64, None
    )
    actions: list[str] = []

    def execute(_connection: object, _data: Path, _logs: Path, **options: object) -> int:
        assert options["admitted"] == admitted
        revalidate = options["revalidate"]
        assert callable(revalidate)
        revalidate()
        action = "verified" if options["verify_only"] else options["exec_command"]
        assert isinstance(action, str)
        actions.append(action)
        return 0

    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(ssh_transport, "usb_present", return_value=True),
        mock.patch.object(runtime, "admit_bundle", return_value=admitted),
        mock.patch.object(live_runtime, "console_connection", side_effect=execute),
        mock.patch.object(build_inputs, "target_workspace_snapshot", return_value=snapshot),
        mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
    ):

        def run() -> None:
            if operation == "verify":
                runtime.verify_booted("phone")
            else:
                runtime.console_target(
                    "phone", keyboard=None, exec_command="id", upload=None, pull=None
                )

        if fresh:
            run()
        else:
            with pytest.raises(SystemExit, match="build output is stale"):
                run()

    assert actions == (["verified" if operation == "verify" else "id"] if fresh else [])


def test_clean_snapshot_can_match_when_device_data_snapshot_differs(
    command_bundle: CommandBundleContext,
) -> None:
    """A clean build remains current when the ordinary snapshot includes phone data."""
    session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
    save_connection(session_root / "session")
    admitted = ValidatedBundle(
        command_bundle.bundle_path, {"target": "phone"}, "a" * 64, "9" * 64, None
    )
    actions: list[str] = []

    def execute(_connection: object, _data: Path, _logs: Path, **options: object) -> int:
        revalidate = options["revalidate"]
        assert callable(revalidate)
        revalidate()
        actions.append("verified")
        return 0

    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(ssh_transport, "usb_present", return_value=True),
        mock.patch.object(runtime, "admit_bundle", return_value=admitted),
        mock.patch.object(live_runtime, "console_connection", side_effect=execute),
        mock.patch.object(
            build_inputs,
            "target_workspace_snapshot",
            side_effect=[WorkspaceSnapshot((), "d" * 64), command_bundle.snapshot],
        ),
        mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
    ):
        runtime.verify_booted("phone")

    assert actions == ["verified"]


def test_verify_uses_the_selected_profile_and_propagates_failure(
    command_bundle: CommandBundleContext,
) -> None:
    """Verification selects the profile's bundle and returns a failed reconnect status."""
    profile = "microsd-uboot"
    bundle = command_bundle.create_generation("b" * 64, profile=profile)
    publish_current_bundle(command_bundle.output, "phone", bundle, profile)
    session_root = checkout_runtime_root(command_bundle.cache, command_bundle.root, "phone")
    save_connection(session_root / "session")
    admitted = ValidatedBundle(bundle, {"target": "phone"}, "b" * 64, "9" * 64, None)

    def execute(_connection: object, _data: Path, logs: Path, **options: object) -> int:
        selected = options["admitted"]
        assert isinstance(selected, ValidatedBundle)
        assert selected.root == bundle
        assert logs == command_bundle.cache / "logs/run/phone/profiles/microsd-uboot"
        revalidate = options["revalidate"]
        assert callable(revalidate)
        revalidate()
        return 7

    with (
        mock.patch.object(common, "ROOT", command_bundle.root),
        mock.patch.object(ssh_transport, "usb_present", return_value=True),
        mock.patch.object(runtime, "admit_bundle", return_value=admitted),
        mock.patch.object(live_runtime, "console_connection", side_effect=execute),
        mock.patch.object(
            build_inputs, "target_workspace_snapshot", return_value=command_bundle.snapshot
        ),
        mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
        pytest.raises(SystemExit) as error,
    ):
        runtime.verify_booted("phone", profile=profile)

    assert error.value.code == 7
