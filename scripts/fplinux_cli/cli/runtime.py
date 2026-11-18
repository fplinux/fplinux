# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate console actions and comparison with the selected running build."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.build import reuse as cli_bundles
from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.manifests.values import TARGET_NAME
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.runtime import live
from fplinux_cli.runtime.bundle import admit_bundle
from fplinux_cli.runtime.data_paths import checkout_runtime_root
from fplinux_cli.runtime.keyboard import forward_keyboard, resolve_keyboard_target
from fplinux_cli.workspace import build_inputs as workspaces

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle


def _checkout_connection(target: str | None) -> tuple[Any, dict[str, Any]]:
    """Choose live credentials before resolving the expected build."""
    scope = checkout_runtime_root(data_root(common.ROOT), common.ROOT)
    candidates = []
    if target is not None:
        candidates.extend(live.load_connections(target, scope / target / "session"))
    elif scope.is_dir():
        for directory in sorted(scope.iterdir()):
            if directory.is_dir() and TARGET_NAME.fullmatch(directory.name) is not None:
                candidates.extend(live.load_connections(directory.name, directory / "session"))
    return live.select_connection(candidates)


def _require_current_source(  # noqa: PLR0913 -- build binding and current selectors are distinct.
    bundle: CurrentBundle,
    manifest: dict[str, Any],
    target: str,
    *,
    profile: str | None,
    build_type: str,
    phone_data: Path | None,
) -> None:
    """Compare one captured expectation with its current causal inputs."""
    latest, _ = runtime_bundle_session.resolve_target_bundle(
        target, profile, build_type=build_type
    )
    if latest.path != bundle.path:
        fail("the selected build changed; run the command again")
    cache = data_root(common.ROOT)
    environment_recipe = definition.environment_recipe_digest()
    environment_state = environment_states.load_environment_state(cache, environment_recipe)
    for include_device_data in (True, False):
        snapshot = workspaces.target_workspace_snapshot(
            target,
            profile,
            build_type=build_type,
            include_device_data=include_device_data,
            phone_data=phone_data,
            selected_data_root=cache,
        )
        identity = cli_bundles.build_identity(snapshot, environment_state, cache)
        if cli_bundles.manifest_matches_identity(manifest, identity):
            return
    fail(
        "build output is stale; rebuild it: "
        + runtime_bundle_session.profile_command("build", target, profile, build_type=build_type)
        + "\nTo inspect the running build instead, use console --allow-other-build."
    )


def console_target(  # noqa: PLR0913 -- public CLI modes remain explicit.
    target: str | None,
    *,
    profile: str | None = None,
    build_type: str = "release",
    keyboard: str | None,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
    phone_data: Path | None = None,
    allow_other_build: bool = False,
) -> None:
    """Open the SSH session, or forward one evdev keyboard over USB."""
    if keyboard is not None:
        target = resolve_keyboard_target(target)
        bundle = None
        if not allow_other_build:
            bundle, _ = runtime_bundle_session.resolve_target_bundle(
                target, profile, build_type=build_type
            )
        config = targets.load_target(target, profile, build_type=build_type)
        forward_keyboard(
            bundle,
            target,
            config,
            keyboard,
            profile=profile,
            phone_data=phone_data,
            build_type=build_type,
            allow_other_build=allow_other_build,
        )
        return
    _console(
        target,
        profile=profile,
        build_type=build_type,
        phone_data=phone_data,
        allow_other_build=allow_other_build,
        exec_command=exec_command,
        upload=upload,
        pull=pull,
    )


def verify_booted(
    target: str | None,
    *,
    profile: str | None = None,
    build_type: str = "release",
    phone_data: Path | None = None,
) -> None:
    """Compare the running kernel identity with the current bundle."""
    _console(
        target, profile=profile, build_type=build_type, phone_data=phone_data, verify_only=True
    )


def _console(  # noqa: PLR0913 -- selection, policy and existing remote actions stay explicit.
    target: str | None,
    *,
    profile: str | None,
    build_type: str,
    phone_data: Path | None,
    allow_other_build: bool = False,
    exec_command: str | None = None,
    upload: list[str] | None = None,
    pull: list[str] | None = None,
    verify_only: bool = False,
) -> None:
    connection = _checkout_connection(target)
    target = str(connection[1]["target"])
    profile = normalize_profile(profile)
    admitted = None
    revalidate: Callable[[], None] | None = None
    if not allow_other_build:
        try:
            bundle, manifest = runtime_bundle_session.resolve_target_bundle(
                target, profile, build_type=build_type
            )
            admitted = admit_bundle(bundle.path)
        except SystemExit as error:
            raise SystemExit(
                f"{error}\nTo inspect the running system, use console --allow-other-build."
            ) from error

        def check_source() -> None:
            _require_current_source(
                bundle,
                manifest,
                target,
                profile=profile,
                build_type=build_type,
                phone_data=phone_data,
            )

        revalidate = check_source

    cache = data_root(common.ROOT)
    data = checkout_runtime_root(cache, common.ROOT, target)
    logs = cache / "logs/run" / target
    if profile is not None:
        logs = logs / "profiles" / profile
    status = live.console_connection(
        connection,
        data,
        logs,
        admitted=admitted,
        phone_data=phone_data,
        exec_command=exec_command,
        upload=upload,
        pull=pull,
        verify_only=verify_only,
        revalidate=revalidate,
    )
    if status:
        raise SystemExit(status)
