# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate console actions and comparison with the selected running build."""

from __future__ import annotations

from fplinux_cli import common
from fplinux_cli.build import reuse as cli_bundles
from fplinux_cli.common import fail
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.runtime.bundle_session import _manifest_device_identity
from fplinux_cli.runtime.keyboard import forward_keyboard
from fplinux_cli.runtime.runner import reconnect_bundle
from fplinux_cli.workspace import build_inputs as workspaces


def console_target(  # noqa: PLR0913 -- public CLI modes remain explicit.
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    keyboard: str | None,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
) -> None:
    """Open the SSH session, or forward one evdev keyboard over USB."""
    bundle, _manifest = runtime_bundle_session.resolve_target_bundle(
        target, profile, build_type=build_type
    )
    if keyboard is None:
        reconnect_bundle(
            bundle,
            target,
            profile=profile,
            exec_command=exec_command,
            upload=upload,
            pull=pull,
        )
        return
    config = targets.load_target(target, profile, build_type=build_type)
    forward_keyboard(bundle, target, config, keyboard, profile=profile)


def verify_booted(target: str, *, profile: str | None = None, build_type: str = "release") -> None:
    """Compare the running kernel identity with the current bundle."""
    profile = normalize_profile(profile)
    bundle, manifest = runtime_bundle_session.resolve_target_bundle(
        target, profile, build_type=build_type
    )
    snapshot = workspaces.target_workspace_snapshot(target, profile, build_type=build_type)
    environment_recipe = definition.environment_recipe_digest()
    environment_state = environment_states.load_environment_state(
        common.ROOT / ".cache", environment_recipe
    )
    identity = cli_bundles.build_identity(snapshot, environment_state, common.ROOT / ".cache")
    current = cli_bundles.manifest_matches_identity(manifest, identity)
    if not current:
        snapshot = workspaces.target_workspace_snapshot(
            target, profile, build_type=build_type, include_device_data=False
        )
        identity = cli_bundles.build_identity(snapshot, environment_state, common.ROOT / ".cache")
        current = cli_bundles.manifest_matches_identity(manifest, identity)
    if not current:
        fail(
            "build output is stale; rebuild it: "
            + runtime_bundle_session.profile_command(
                "build", target, profile, build_type=build_type
            )
        )
    device_identity = _manifest_device_identity(manifest)
    reconnect_bundle(bundle, target, profile=profile, exec_command="true")
    print(f"verify: the phone runs the current {build_type} build ({device_identity[:16]})")
