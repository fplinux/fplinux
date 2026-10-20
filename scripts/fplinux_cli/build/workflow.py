# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate a target build through the pinned environment."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.bundles import CurrentBundle, discard_superseded_bundle_generations
from fplinux_cli.build import DEFAULT_BUILD_MEMORY_MIB
from fplinux_cli.build import reuse as bundles_commands
from fplinux_cli.cache.prune import operations as cache_prune
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.environment import definition
from fplinux_cli.environment import kern as environment_kern
from fplinux_cli.environment import state as environment_states
from fplinux_cli.environment.runtime import environment_arguments, require_environment
from fplinux_cli.manifests import releases
from fplinux_cli.reporting.process import silence_broken_pipe
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace import build_inputs as workspace_build_inputs
from fplinux_cli.workspace import capture as workspace_capture
from fplinux_cli.workspace import staging as workspace_staging

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.workspace.capture import WorkspaceSnapshot


def ensure_build_directory(path: Path) -> Path:
    """Create one exact build mount root without accepting a symlink or file."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"invalid build cache directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        fail(f"invalid build cache directory: {path}")
    return path


def prepare_host_tool(  # noqa: PLR0913 -- operation inputs and environment policy stay explicit.
    target: str,
    platform: str,
    tool: str,
    output: Path,
    *,
    offline: bool,
    reporter: RunReporter,
) -> None:
    """Build one cached host utility in the pinned environment without fitted device data."""
    snapshot = workspace_capture.workspace_snapshot(
        workspace_build_inputs.target_build_source_files(target)
    )
    kern, environment_rootfs, environment_state = require_environment(
        offline=offline, reporter=reporter
    )
    cache = common.ROOT / ".cache"
    downloads = ensure_build_directory(cache / "downloads")
    host_tools = ensure_build_directory(cache / "host-tools")
    workspace = workspace_staging.stage_workspace_snapshot(snapshot)
    try:
        container_logs = ensure_build_directory(reporter.root / "host-tools")
        log_environment = reporter.container_environment("/logs")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] += "/host-tools"
        command = [
            kern,
            "box",
            environment_kern.kern_box_name("host-tools"),
            *environment_arguments(environment_rootfs),
            "--read-only",
            "--uid-range",
            "--privileged",
            "--network",
            "none" if offline else "host",
            "--tmpfs",
            "/tmp:1g",  # noqa: S108 -- container tmpfs.
            "--volume",
            f"{downloads}:/cache/downloads",
            "--volume",
            f"{host_tools}:/cache/host-tools",
            "--volume",
            f"{output}:/out",
            "--volume",
            f"{container_logs}:/logs",
            "--volume",
            f"{workspace}:/workspace:ro",
            *[
                argument
                for key, value in log_environment.items()
                for argument in ("--env", f"{key}={value}")
            ],
            "--env",
            "HOME=/tmp/fplinux-home",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            f"FPLINUX_ENVIRONMENT_RECIPE={environment_state.recipe}",
            "--env",
            f"FPLINUX_ENVIRONMENT_CONTENT={environment_state.content}",
            "--workdir",
            "/workspace",
            "--init",
            "--quiet",
            "--",
            "python3",
            "-m",
            "fplinux_cli.build.host_tools.host",
            platform,
            tool,
        ]
        with reporter.stage("host-tools", passthrough=True, show_tail=False) as stage:
            stage.run(command, env=environment_kern.kern_environment())
    finally:
        workspace_staging.discard_staged_workspace_snapshot(snapshot, workspace)


def _build_command(  # noqa: PLR0913
    kern: str,
    *,
    target: str,
    jobs: int,
    environment_rootfs: Path,
    offline: bool,
    snapshot: WorkspaceSnapshot,
    workspace: Path,
    downloads: Path,
    ccache: Path,
    host_tools: Path,
    apk_signing: Path,
    apks: Path,
    rootfs: Path,
    linux: Path,
    output: Path,
    logs: Path,
    log_environment: dict[str, str],
    environment_recipe: str,
    environment_content: str,
    memory_mib: int = DEFAULT_BUILD_MEMORY_MIB,
    profile: str | None = None,
    build_type: str = "release",
) -> list[str]:
    """Return the exact target-build argv with only narrow explicit mounts."""
    log_arguments = [
        argument
        for key, value in log_environment.items()
        for argument in ("--env", f"{key}={value}")
    ]
    return [
        kern,
        "box",
        environment_kern.kern_box_name("build"),
        *environment_arguments(environment_rootfs),
        "--read-only",
        "--uid-range",
        "--privileged",
        "--memory",
        f"{memory_mib}m",
        "--network",
        "none" if offline else "host",
        "--tmpfs",
        "/tmp:8g",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{downloads}:/cache/downloads",
        "--volume",
        f"{ccache}:/cache/ccache",
        "--volume",
        f"{host_tools}:/cache/host-tools",
        "--volume",
        f"{apk_signing}:/cache/apk-signing",
        "--volume",
        f"{apks}:/cache/apks",
        "--volume",
        f"{rootfs}:/cache/rootfs",
        "--volume",
        f"{linux}:/cache/linux",
        "--volume",
        f"{output}:/out",
        "--volume",
        f"{logs}:/logs",
        "--volume",
        f"{workspace}:/workspace:ro",
        *log_arguments,
        "--env",
        "HOME=/tmp/fplinux-home",
        "--env",
        "PYTHONPATH=/workspace/scripts",
        "--env",
        f"FPLINUX_ENVIRONMENT_RECIPE={environment_recipe}",
        "--env",
        f"FPLINUX_ENVIRONMENT_CONTENT={environment_content}",
        "--env",
        f"FPLINUX_WORKSPACE_DIGEST={snapshot.recipe}",
        "--workdir",
        "/workspace",
        "--init",
        "--quiet",
        "--",
        "python3",
        "-m",
        "fplinux_cli.build",
        "--target",
        target,
        "--build-type",
        build_type,
        *(["--profile", profile] if profile is not None else []),
        "--jobs",
        str(jobs),
    ]


def _print_build_result(  # noqa: PLR0913 -- result and selected build context stay explicit.
    target: str,
    bundle: CurrentBundle,
    release: dict[str, Any],
    *,
    cached: bool,
    profile: str | None = None,
    build_type: str = "release",
) -> None:
    image = bundle.path / release["image"]
    if image.is_symlink() or not image.is_file():
        fail(f"current bundle image is missing or invalid: {image}")
    suffix = " (cached)" if cached else ""
    try:
        label = f"build {target} --build-type {build_type}"
        if profile is not None:
            label += f" --profile {profile}"
        print(f"{label}: OK{suffix}", flush=True)
        print(f"output: {bundle.path.relative_to(common.ROOT)}", flush=True)
        print(f"ramboot.bin SHA256: {sha256_file(image)}", flush=True)
    except BrokenPipeError:
        silence_broken_pipe(sys.stdout)


def build(  # noqa: PLR0913 -- CLI options and caller-owned reporting remain explicit.
    target: str,
    jobs: int,
    *,
    memory_mib: int = DEFAULT_BUILD_MEMORY_MIB,
    profile: str | None = None,
    build_type: str = "release",
    include_device_data: bool = True,
    verbose: bool = False,
    offline: bool = False,
    reporter: RunReporter | None = None,
) -> None:
    """Build or reuse a bundle, leaving a supplied reporter to its caller."""
    own_reporter = reporter is None
    if jobs < 1:
        fail("--jobs must be positive")
    release = releases.load_release(target)
    snapshot = workspace_build_inputs.target_workspace_snapshot(
        target, profile, build_type=build_type, include_device_data=include_device_data
    )
    environment_lock = definition.load_environment_lock()
    environment_recipe = definition.environment_recipe_digest(environment_lock)
    cache = common.ROOT / ".cache"
    environment_state = environment_states.load_environment_state(cache, environment_recipe)
    identity = bundles_commands.build_identity(snapshot, environment_state, cache)
    current = bundles_commands.matching_target_bundle(
        target,
        identity,
        release["image"],
        profile,
        build_type=build_type,
    )
    if current is not None:
        bundle, _manifest = current
        discard_superseded_bundle_generations(
            cache / "out",
            target,
            bundle,
            profile,
            build_type=build_type,
        )
        cache_prune.discard_obsolete_rootfs(cache)
        cache_prune.discard_obsolete_apks(cache)
        if reporter is None:
            reporter = RunReporter.create(
                "build",
                target=bundles_commands.profile_log_target(target, profile),
                verbose=verbose,
                build_type=build_type,
            )
        _print_build_result(
            target, bundle, release, cached=True, profile=profile, build_type=build_type
        )
        if own_reporter:
            reporter.finish()
        if profile is not None:
            cache_prune.discard_superseded_profile_logs(
                cache,
                "build",
                profile=profile,
                target=target,
            )
        return

    if reporter is None:
        reporter = RunReporter.create(
            "build",
            target=bundles_commands.profile_log_target(target, profile),
            verbose=verbose,
            build_type=build_type,
        )
    kern, environment_rootfs, current_environment = require_environment(
        offline=offline, reporter=reporter
    )
    apk_signing = ensure_build_directory(cache / "apk-signing")
    downloads = ensure_build_directory(cache / "downloads")
    ccache = ensure_build_directory(cache / "ccache")
    host_tools = ensure_build_directory(cache / "host-tools")
    apks = ensure_build_directory(cache / "apks")
    rootfs = ensure_build_directory(cache / "rootfs")
    linux = ensure_build_directory(cache / "linux")
    output = ensure_build_directory(cache / "out")
    with reporter.stage("workspace"):
        workspace = workspace_staging.stage_workspace_snapshot(snapshot)
    try:
        container_logs = ensure_build_directory(reporter.root / "container")
        log_environment = reporter.container_environment("/logs")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/container"
        )
        with reporter.stage("container", passthrough=True, show_tail=False) as stage:
            try:
                stage.run(
                    _build_command(
                        kern,
                        target=target,
                        profile=profile,
                        build_type=build_type,
                        jobs=jobs,
                        memory_mib=memory_mib,
                        environment_rootfs=environment_rootfs,
                        offline=offline,
                        snapshot=snapshot,
                        workspace=workspace,
                        downloads=downloads,
                        ccache=ccache,
                        host_tools=host_tools,
                        apk_signing=apk_signing,
                        apks=apks,
                        rootfs=rootfs,
                        linux=linux,
                        output=output,
                        logs=container_logs,
                        log_environment=log_environment,
                        environment_recipe=current_environment.recipe,
                        environment_content=current_environment.content,
                    ),
                    env=environment_kern.kern_environment(),
                )
            except SystemExit as error:
                if error.code == 137:
                    environment_kern.report_kern_memory_failure(
                        stage,
                        memory_mib=memory_mib,
                        advice=(
                            "Increase build --memory-mib if the host has enough available memory."
                        ),
                    )
                raise
        identity = bundles_commands.build_identity(snapshot, current_environment, cache)
        current = bundles_commands.matching_target_bundle(
            target,
            identity,
            release["image"],
            profile,
            build_type=build_type,
        )
        if current is None:
            fail("build completed without publishing an exact valid current bundle")
        bundle, _manifest = current
        discard_superseded_bundle_generations(
            output,
            target,
            bundle,
            profile,
            build_type=build_type,
        )
        cache_prune.discard_obsolete_rootfs(cache)
        cache_prune.discard_obsolete_apks(cache)
        _print_build_result(
            target, bundle, release, cached=False, profile=profile, build_type=build_type
        )
        if own_reporter:
            reporter.finish()
        if profile is not None:
            cache_prune.discard_superseded_profile_logs(
                cache,
                "build",
                profile=profile,
                target=target,
            )
    finally:
        workspace_staging.discard_staged_workspace_snapshot(snapshot, workspace)
