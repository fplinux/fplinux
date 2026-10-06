# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate scoped checks and their causal cache receipts."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.cache.prune.operations import discard_superseded_profile_logs
from fplinux_cli.common import ROOT, fail
from fplinux_cli.environment.image_state import load_image_state
from fplinux_cli.environment.images import container_image_recipe_digest, load_container_lock
from fplinux_cli.environment.kern import kern_box_name, kern_environment
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace.quality_inputs import quality_workspace_snapshot
from fplinux_cli.workspace.staging import (
    discard_staged_quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

from .git import check_git_diff
from .inputs import check_scope_closure_digest
from .receipts import (
    CheckReceiptRecipe,
    check_orchestration_recipe_digest,
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
)
from .runtime import prepare_quality_image, run_quality_command
from .scopes import SOURCE_CHECK_SCOPES, analyzer_cache_names, resolve_check_scopes

if TYPE_CHECKING:
    from pathlib import Path

_KERNEL_PREPARE_TIMEOUT = 90 * 60


_KERNEL_ANALYSIS_TIMEOUT = 90 * 60


def _run_missing_checks(  # noqa: PLR0913 -- container boundaries are explicit.
    *,
    reporter: RunReporter,
    cache: Path,
    missing: tuple[str, ...],
    analyzer_cache: dict[str, Path],
    workspace: Path,
    kern: str,
    image: str,
    recipes: dict[str, CheckReceiptRecipe],
    profile: str | None,
    build_type: str,
    jobs: int,
) -> None:
    """Run cache-missing source and kernel checks against one disposable workspace."""
    container_logs = reporter.root / "containers"
    if container_logs.is_symlink() or (container_logs.exists() and not container_logs.is_dir()):
        fail(f"invalid checker container log directory: {container_logs}")
    container_logs.mkdir(parents=True, exist_ok=True)
    source_scopes = [scope for scope in missing if scope in SOURCE_CHECK_SCOPES]
    if source_scopes:
        with reporter.stage("source", passthrough=True, show_tail=False) as stage:
            run_quality_command(
                stage,
                workspace,
                ["python3", "/workspace/scripts/check.py", *source_scopes],
                kern=kern,
                image=image,
            )
        for scope in source_scopes:
            publish_success_receipt(cache, recipes[scope])

    if "kernel" not in missing:
        return
    environment = kern_environment()
    log_mount = ["--volume", f"{container_logs}:/logs"]
    log_environment = reporter.container_environment("/logs/kernel")
    log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
        f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/containers/kernel"
    )
    log_arguments = [
        argument
        for key, value in log_environment.items()
        for argument in ("--env", f"{key}={value}")
    ]
    common = [
        "--image",
        image,
        "--pull",
        "never",
        "--read-only",
        "--tmpfs",
        "/tmp:8g",  # noqa: S108 -- container tmpfs.
        "--no-uid-range",
        "--volume",
        f"{workspace}:/workspace:ro",
        *log_mount,
        *log_arguments,
        "--env",
        "HOME=/tmp",
        "--env",
        "PYTHONPATH=/workspace/scripts",
        "--env",
        "PYTHONDONTWRITEBYTECODE=1",
        "--workdir",
        "/workspace",
        "--init",
        "--quiet",
    ]
    with reporter.stage("kernel-prepare", passthrough=True, show_tail=False) as stage:
        stage.run(
            [
                kern,
                "box",
                kern_box_name("kernel-prepare"),
                *common,
                "--network",
                "host",
                "--volume",
                f"{analyzer_cache['downloads']}:/cache/downloads",
                "--volume",
                f"{analyzer_cache['linux']}:/cache/linux",
                "--",
                "python3",
                "-m",
                "fplinux_cli.quality.kernel",
                "prepare",
                *([] if profile is None else ["--profile", profile]),
                "--build-type",
                build_type,
            ],
            env=environment,
            timeout=_KERNEL_PREPARE_TIMEOUT,
        )
    with reporter.stage("kernel-analysis", passthrough=True, show_tail=False) as stage:
        stage.run(
            [
                kern,
                "box",
                kern_box_name("kernel-analysis"),
                *common,
                "--network",
                "none",
                "--volume",
                f"{analyzer_cache['analysis']}:/cache/analysis",
                "--volume",
                f"{analyzer_cache['downloads']}:/cache/downloads:ro",
                "--volume",
                f"{analyzer_cache['linux']}:/cache/linux:ro",
                "--",
                "python3",
                "-m",
                "fplinux_cli.quality.kernel",
                "check",
                "--jobs",
                str(jobs),
                *([] if profile is None else ["--profile", profile]),
                "--build-type",
                build_type,
            ],
            env=environment,
            timeout=_KERNEL_ANALYSIS_TIMEOUT,
        )
    publish_success_receipt(cache, recipes["kernel"])


def check(  # noqa: PLR0913 -- public check options stay explicit.
    scopes: list[str],
    *,
    verbose: bool = False,
    no_cache: bool = False,
    profile: str | None = None,
    build_type: str = "release",
    jobs: int = 1,
) -> None:
    if not isinstance(jobs, int) or isinstance(jobs, bool) or jobs < 1:
        fail("--jobs must be a positive integer")
    profile = normalize_profile(profile)
    selected = resolve_check_scopes(scopes)
    if jobs > 1 and verbose:
        fail("--verbose cannot be combined with --jobs greater than 1")

    reporter = RunReporter.create(
        "check",
        target=None if profile is None else f"profiles/{profile}",
        verbose=verbose,
    )
    if "repository" in selected:
        check_git_diff(reporter)
    if selected == ("repository",):
        print("check: OK")
        reporter.finish()
        return

    with reporter.stage("workspace-snapshot"):
        snapshot = quality_workspace_snapshot(enforce_source_policy="source" in selected)

    cache = ROOT / ".cache"
    cacheable_scopes = tuple(scope for scope in selected if scope != "repository")
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    orchestration_recipe = check_orchestration_recipe_digest(image_recipe)

    def receipt_recipes(image_generation: str) -> dict[str, CheckReceiptRecipe]:
        return {
            scope: check_scope_receipt_recipe(
                scope,
                check_scope_closure_digest(
                    scope, snapshot, profile=profile, build_type=build_type
                ),
                image_generation=image_generation,
                orchestration_recipe=orchestration_recipe,
                profile=profile,
                build_type=build_type,
            )
            for scope in cacheable_scopes
        }

    cached_image = load_image_state(cache, image_recipe)
    if cached_image is not None:
        recipes = receipt_recipes(cached_image.image_generation)
        missing = tuple(
            scope
            for scope in cacheable_scopes
            if no_cache or not receipt_matches(cache, recipes[scope])
        )
        if not missing:
            for scope in cacheable_scopes:
                print(f"check cache: hit ({scope})")
            print("check: OK")
            reporter.finish()
            return

    kern, image, current_image = prepare_quality_image(reporter, container_lock, image_recipe)

    recipes = receipt_recipes(current_image.image_generation)
    missing = tuple(
        scope
        for scope in cacheable_scopes
        if no_cache or not receipt_matches(cache, recipes[scope])
    )
    for scope in cacheable_scopes:
        if scope not in missing:
            print(f"check cache: hit ({scope})")
    if not missing:
        print("check: OK")
        reporter.finish()
        return

    analyzer_cache: dict[str, Path] = {}
    for name in analyzer_cache_names(missing):
        source = cache / name
        if source.is_symlink() or (source.exists() and not source.is_dir()):
            fail(f"invalid analyzer cache path: {source}")
        source.mkdir(parents=True, exist_ok=True)
        analyzer_cache[name] = source

    with reporter.stage("workspace"):
        workspace = stage_quality_workspace_snapshot(snapshot)

    try:
        _run_missing_checks(
            reporter=reporter,
            cache=cache,
            missing=missing,
            analyzer_cache=analyzer_cache,
            workspace=workspace,
            kern=kern,
            image=image,
            recipes=recipes,
            profile=profile,
            build_type=build_type,
            jobs=jobs,
        )
    finally:
        discard_staged_quality_workspace_snapshot(snapshot, workspace)
    print("check: OK")
    reporter.finish()
    if profile is not None:
        discard_superseded_profile_logs(cache, "check", profile=profile)
