# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate a target build through the pinned environment."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli import image_state as image_states
from fplinux_cli import prune as cache_prune
from fplinux_cli import workspace as workspaces
from fplinux_cli.bundle_state import CurrentBundle, discard_superseded_bundle_generations
from fplinux_cli.cli import bundles as bundles_commands
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.environment import images
from fplinux_cli.environment import kern as kern_env
from fplinux_cli.manifests import releases
from fplinux_cli.output import RunReporter, silence_broken_pipe

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.workspace import WorkspaceSnapshot


def ensure_build_directory(path: Path) -> Path:
    """Create one exact build mount root without accepting a symlink or file."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"invalid build cache directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        fail(f"invalid build cache directory: {path}")
    return path


def _build_container_command(  # noqa: PLR0913
    kern: str,
    *,
    target: str,
    jobs: int,
    image: str,
    offline: bool,
    snapshot: WorkspaceSnapshot,
    workspace: Path,
    downloads: Path,
    apk_signing: Path,
    apks: Path,
    rootfs: Path,
    linux: Path,
    output: Path,
    logs: Path,
    log_environment: dict[str, str],
    image_recipe: str,
    image_generation: str,
    profile: str | None = None,
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
        kern_env.kern_box_name("build"),
        "--image",
        image,
        "--pull",
        "never",
        "--read-only",
        "--privileged",
        "--memory",
        "2g",
        "--network",
        "none" if offline else "host",
        "--tmpfs",
        "/tmp:8g",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{downloads}:/cache/downloads",
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
        f"FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE={image_recipe}",
        "--env",
        f"FPLINUX_CONTAINER_IMAGE_GENERATION={image_generation}",
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
        *(["--profile", profile] if profile is not None else []),
        "--jobs",
        str(jobs),
    ]


def _print_build_result(
    target: str,
    bundle: CurrentBundle,
    release: dict[str, Any],
    *,
    cached: bool,
    profile: str | None = None,
) -> None:
    image = bundle.path / release["image"]
    if image.is_symlink() or not image.is_file():
        fail(f"current bundle image is missing or invalid: {image}")
    suffix = " (cached)" if cached else ""
    try:
        label = f"build {target}"
        if profile is not None:
            label += f" --profile {profile}"
        print(f"{label}: OK{suffix}", flush=True)
        print(f"output: {bundle.path.relative_to(common.ROOT)}", flush=True)
        print(f"ramboot.bin SHA256: {sha256_file(image)}", flush=True)
    except BrokenPipeError:
        silence_broken_pipe(sys.stdout)


def build(
    target: str,
    jobs: int,
    *,
    profile: str | None = None,
    verbose: bool = False,
    offline: bool = False,
) -> None:
    """Build or reuse a bundle and report the command result."""
    if jobs < 1:
        fail("--jobs must be positive")
    release = releases.load_release(target)
    snapshot = workspaces.target_workspace_snapshot(target, profile)
    container_lock = images.load_container_lock()
    image_recipe = images.container_image_recipe_digest(container_lock)
    cache = common.ROOT / ".cache"
    image_state = image_states.load_image_state(cache, image_recipe)
    identity = bundles_commands.build_identity(snapshot, image_state, cache)
    current = bundles_commands.matching_target_bundle(
        target,
        identity,
        release["image"],
        profile,
    )
    if current is not None:
        bundle, _manifest = current
        discard_superseded_bundle_generations(
            cache / "out",
            target,
            bundle,
            profile,
        )
        cache_prune.discard_obsolete_rootfs(cache)
        cache_prune.discard_obsolete_apks(cache)
        reporter = RunReporter.create(
            "build",
            target=bundles_commands.profile_log_target(target, profile),
            verbose=verbose,
        )
        _print_build_result(target, bundle, release, cached=True, profile=profile)
        reporter.finish()
        if profile is not None:
            cache_prune.discard_superseded_profile_logs(
                cache,
                "build",
                profile=profile,
                target=target,
            )
        return

    reporter = RunReporter.create(
        "build",
        target=bundles_commands.profile_log_target(target, profile),
        verbose=verbose,
    )
    image = images.container_image_reference(container_lock, image_recipe)
    if not kern_env.kern_available(container_lock):
        if offline:
            fail(
                "offline build requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        current_image = kern_env.setup(
            reporter=reporter, lock=container_lock, image_recipe=image_recipe
        )
    else:
        current_image = None
    kern = kern_env.require_kern(container_lock)
    inspected_image = kern_env.current_image_state(kern, image, image_recipe)
    if inspected_image is None:
        if offline:
            fail(
                "offline build requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        current_image = kern_env.setup(
            reporter=reporter, lock=container_lock, image_recipe=image_recipe
        )
    elif current_image is None:
        current_image = kern_env.publish_current_image_state(
            kern,
            image,
            image_recipe,
            state=inspected_image,
        )
    apk_signing = ensure_build_directory(cache / "apk-signing")
    downloads = ensure_build_directory(cache / "downloads")
    apks = ensure_build_directory(cache / "apks")
    rootfs = ensure_build_directory(cache / "rootfs")
    linux = ensure_build_directory(cache / "linux")
    output = ensure_build_directory(cache / "out")
    with reporter.stage("workspace"):
        workspace = workspaces.stage_workspace_snapshot(snapshot)
    try:
        container_logs = ensure_build_directory(reporter.root / "container")
        log_environment = reporter.container_environment("/logs")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/container"
        )
        with reporter.stage("container", passthrough=True, show_tail=False) as stage:
            stage.run(
                _build_container_command(
                    kern,
                    target=target,
                    profile=profile,
                    jobs=jobs,
                    image=image,
                    offline=offline,
                    snapshot=snapshot,
                    workspace=workspace,
                    downloads=downloads,
                    apk_signing=apk_signing,
                    apks=apks,
                    rootfs=rootfs,
                    linux=linux,
                    output=output,
                    logs=container_logs,
                    log_environment=log_environment,
                    image_recipe=image_recipe,
                    image_generation=current_image.image_generation,
                ),
                env=kern_env.kern_environment(),
            )
        identity = bundles_commands.build_identity(snapshot, current_image, cache)
        current = bundles_commands.matching_target_bundle(
            target,
            identity,
            release["image"],
            profile,
        )
        if current is None:
            fail("build completed without publishing an exact valid current bundle")
        bundle, _manifest = current
        discard_superseded_bundle_generations(
            output,
            target,
            bundle,
            profile,
        )
        cache_prune.discard_obsolete_rootfs(cache)
        cache_prune.discard_obsolete_apks(cache)
        _print_build_result(target, bundle, release, cached=False, profile=profile)
        reporter.finish()
        if profile is not None:
            cache_prune.discard_superseded_profile_logs(
                cache,
                "build",
                profile=profile,
                target=target,
            )
    finally:
        workspaces.discard_staged_workspace_snapshot(snapshot, workspace)
