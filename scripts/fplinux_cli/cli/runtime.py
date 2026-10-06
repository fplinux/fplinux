# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate console actions and comparison with the selected running build."""

from __future__ import annotations

from fplinux_cli import common
from fplinux_cli.cli import bundles as cli_bundles
from fplinux_cli.common import fail
from fplinux_cli.environment import image_state as image_states
from fplinux_cli.environment import images
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.runtime.bundle_session import _current_ssh_session, _manifest_device_identity
from fplinux_cli.runtime.keyboard import forward_keyboard
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
    config = targets.load_target(target, profile, build_type=build_type)
    bundle, manifest = runtime_bundle_session.resolve_target_bundle(
        target, profile, build_type=build_type
    )
    if keyboard is None:
        ssh_transport, session = _current_ssh_session(bundle, manifest, target)
        if exec_command is not None:
            result = ssh_transport.run_remote(session, exec_command)
            if result.returncode:
                raise SystemExit(result.returncode)
            return
        if upload is not None:
            ssh_transport.upload(session, upload[0], upload[1])
            return
        if pull is not None:
            ssh_transport.pull(session, pull[0], pull[1])
            return
        ssh_transport.open_shell(session)
        return
    forward_keyboard(
        bundle, manifest, target, config, keyboard, profile=profile, build_type=build_type
    )


def verify_booted(target: str, *, profile: str | None = None, build_type: str = "release") -> None:
    """Compare the running kernel identity with the current bundle."""
    profile = normalize_profile(profile)
    bundle, manifest = runtime_bundle_session.resolve_target_bundle(
        target, profile, build_type=build_type
    )
    snapshot = workspaces.target_workspace_snapshot(target, profile, build_type=build_type)
    image_recipe = images.container_image_recipe_digest()
    image_state = image_states.load_image_state(common.ROOT / ".cache", image_recipe)
    identity = cli_bundles.build_identity(snapshot, image_state, common.ROOT / ".cache")
    if not cli_bundles.manifest_matches_identity(manifest, identity):
        fail(
            "build output is stale; rebuild it: "
            + runtime_bundle_session.profile_command(
                "build", target, profile, build_type=build_type
            )
        )
    device_identity = _manifest_device_identity(manifest)
    _current_ssh_session(bundle, manifest, target)
    print(f"verify: the phone runs the current {build_type} build ({device_identity[:16]})")
