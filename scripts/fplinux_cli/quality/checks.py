# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate scoped checks and their causal cache receipts."""

from __future__ import annotations

import re
import tomllib
from pathlib import Path, PurePath

from fplinux_cli import alpine_state
from fplinux_cli.checkreceipts import (
    CheckReceiptRecipe,
    check_closure_entries_digest,
    check_orchestration_recipe_digest,
    publish_success_receipt,
    receipt_matches,
)
from fplinux_cli.common import ROOT, fail
from fplinux_cli.environment.images import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.environment.kern import (
    current_image_state,
    kern_available,
    kern_box_name,
    kern_environment,
    publish_current_image_state,
    require_kern,
    setup,
)
from fplinux_cli.image_state import load_image_state
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.output import RunReporter
from fplinux_cli.prune import discard_superseded_profile_logs
from fplinux_cli.source_formats import (
    is_explicit_json_source,
    is_javascript_source,
    is_posix_shell_fragment,
    shell_dialect,
)
from fplinux_cli.workspace import (
    WorkspaceFile,
    WorkspaceSnapshot,
    discard_staged_quality_workspace_snapshot,
    quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

from .git import check_git_diff

CHECK_SCOPES = (
    "repository",
    "source",
    "container",
    "metadata",
    "docs",
    "spelling",
    "secrets",
    "licenses",
    "python",
    "shell",
    "alpine",
    "c",
    "kernel",
)


SOURCE_CHECK_SCOPES = CHECK_SCOPES[1:-1]


_SOURCE_CHECK_TIMEOUT = 2 * 60 * 60


_KERNEL_PREPARE_TIMEOUT = 90 * 60


_KERNEL_ANALYSIS_TIMEOUT = 90 * 60


_QUOTED_C_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)


_PRETTIER_CONFIGURATION_NAMES = frozenset(
    {
        ".prettierrc",
        ".prettierrc.cjs",
        ".prettierrc.cts",
        ".prettierrc.js",
        ".prettierrc.json",
        ".prettierrc.json5",
        ".prettierrc.mjs",
        ".prettierrc.mts",
        ".prettierrc.toml",
        ".prettierrc.ts",
        ".prettierrc.yaml",
        ".prettierrc.yml",
        "prettier.config.cjs",
        "prettier.config.cts",
        "prettier.config.js",
        "prettier.config.mjs",
        "prettier.config.mts",
        "prettier.config.ts",
    }
)


_EXECUTABLE_PRETTIER_CONFIGURATION_NAMES = frozenset(
    name
    for name in _PRETTIER_CONFIGURATION_NAMES
    if Path(name).suffix in {".cjs", ".cts", ".js", ".mjs", ".mts", ".ts"}
)


_CHECK_IMPLEMENTATION = frozenset(
    {
        "scripts/check.py",
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/alpine_state.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/environment/__init__.py",
        "scripts/fplinux_cli/environment/images.py",
        "scripts/fplinux_cli/quality/__init__.py",
        "scripts/fplinux_cli/quality/source_policy.py",
        "scripts/fplinux_cli/identity.py",
        "scripts/fplinux_cli/identity_codegen.py",
        "scripts/fplinux_cli/output.py",
        "scripts/fplinux_cli/source_formats.py",
    }
)


_KERNEL_IMPLEMENTATION = frozenset(
    {
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/alpine_builder.py",
        "scripts/fplinux_cli/alpine_state.py",
        "scripts/fplinux_cli/build_env.py",
        "scripts/fplinux_cli/bundle_state.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/environment/__init__.py",
        "scripts/fplinux_cli/environment/images.py",
        "scripts/fplinux_cli/quality/__init__.py",
        "scripts/fplinux_cli/quality/source_policy.py",
        "scripts/fplinux_cli/device_state.py",
        "scripts/fplinux_cli/device_tree.py",
        "scripts/fplinux_cli/identity.py",
        "scripts/fplinux_cli/identity_codegen.py",
        "scripts/fplinux_cli/kbuild_state.py",
        "scripts/fplinux_cli/kernelcheck.py",
        "scripts/fplinux_cli/kernel_patches.py",
        "scripts/fplinux_cli/workspace.py",
        "scripts/fplinux_cli/linux_state.py",
        "scripts/fplinux_cli/output.py",
        "scripts/fplinux_cli/profile_layout.py",
    }
)


def resolve_check_scopes(scopes: list[str]) -> tuple[str, ...]:
    """Validate, deduplicate and canonicalize a check selection."""
    requested = set(scopes)
    unknown = requested.difference(CHECK_SCOPES)
    if unknown:
        fail(f"unknown check scope: {', '.join(sorted(unknown))}")
    return tuple(scope for scope in CHECK_SCOPES if not scopes or scope in requested)


def analyzer_cache_names(scopes: tuple[str, ...]) -> tuple[str, ...]:
    """Return analyzer caches required by the selected scopes."""
    required: set[str] = set()
    if "kernel" in scopes:
        required.update(("analysis", "downloads", "linux"))
    return tuple(name for name in ("analysis", "downloads", "linux") if name in required)


def _is_shell_source(file: WorkspaceFile) -> bool:
    if is_posix_shell_fragment(file.path):
        return True
    if Path(file.path).suffix not in {"", ".initd", ".sh"}:
        return False
    first_line = file.contents.splitlines()[:1]
    if not first_line:
        return False
    return shell_dialect(first_line[0]) is not None


def _is_prettier_configuration(path: str) -> bool:
    name = Path(path).name
    return (
        name in {".gitignore", ".prettierignore", "package.yaml"}
        or name in _PRETTIER_CONFIGURATION_NAMES
    )


def _source_scope_uses_file(  # noqa: PLR0911
    scope: str, file: WorkspaceFile
) -> bool:
    """Return whether one captured file can affect the selected source scope."""
    path = PurePath(file.path)
    name = path.name
    suffix = path.suffix
    parts = path.parts
    if file.path in _CHECK_IMPLEMENTATION or (
        file.path.startswith("scripts/fplinux_cli/manifests/") and suffix == ".py"
    ):
        return True
    if scope in {
        "source",
        "docs",
        "spelling",
        "secrets",
        "licenses",
        "python",
    }:
        return True
    if scope == "container":
        return name in {".kernignore", "Containerfile"} or name.startswith(".hadolint")
    if scope == "metadata":
        return (
            suffix == ".toml"
            or (suffix in {".json", ".jsonc"} and name != "package-lock.json")
            or is_javascript_source(file.path)
            or is_explicit_json_source(file.path)
            or name == ".editorconfig"
            or _is_prettier_configuration(file.path)
        )
    if scope == "shell":
        return _is_shell_source(file) or name in {".editorconfig", ".shellcheckrc"}
    if scope == "alpine":
        return (
            file.path in {"alpine.lock.toml", "alpine/abuild.conf"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
            or (len(parts) == 4 and parts[:2] == ("alpine", "aports") and name == "APKBUILD")
        )
    if scope == "c":
        return (
            name in {".clang-format", ".clang-format-ignore", "_clang-format"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
        )
    fail(f"check scope does not support a source closure: {scope}")
    return False


def _c_scope_paths(snapshot: WorkspaceSnapshot) -> set[str]:
    """Resolve the same userspace/bootstrap C inputs and their quoted headers."""
    by_path = {file.path: file for file in snapshot.files}
    selected = {file.path for file in snapshot.files if _source_scope_uses_file("c", file)}
    for file in snapshot.files:
        path = PurePath(file.path)
        if path.suffix in {".c", ".h"} and (
            path.parts[:2] == ("alpine", "aports")
            or file.path in alpine_state.SHARED_APORT_SOURCE_PATHS
            or path.parts[0] == "tests"
            or (
                len(path.parts) >= 4
                and path.parts[0] in {"platforms", "targets"}
                and path.parts[2] in {"common", "uboot"}
            )
        ):
            selected.add(file.path)
        if path.suffix in {".c", ".h"} and "bootstrap" in path.parts:
            selected.add(file.path)

    for file in snapshot.files:
        path = PurePath(file.path)
        if len(path.parts) != 3 or path.parts[0] != "platforms" or path.name != "platform.toml":
            continue
        try:
            manifest = tomllib.loads(file.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        tools = manifest.get("host", {}).get("tools", [])
        if not isinstance(tools, list):
            continue
        for recipe in tools:
            if isinstance(recipe, dict) and recipe.get("type") == "cc-libusb":
                source = recipe.get("source")
                if isinstance(source, str) and source in by_path:
                    selected.add(source)

    pending = list(selected)
    while pending:
        relative = pending.pop()
        source = by_path.get(relative)
        if source is None or PurePath(relative).suffix not in {".c", ".h"}:
            continue
        for raw_include in _QUOTED_C_INCLUDE.findall(source.contents):
            try:
                include = raw_include.decode("utf-8")
            except UnicodeDecodeError:
                continue
            candidates = (
                (PurePath(relative).parent / include).as_posix(),
                PurePath(include).as_posix(),
            )
            for candidate in candidates:
                if candidate in by_path and candidate not in selected:
                    selected.add(candidate)
                    pending.append(candidate)
                    break
    return selected


def _linux_manifest_sources(linux: object, *, base: PurePath) -> set[str]:
    """Return the captured Linux inputs explicitly named by one manifest."""
    if not isinstance(linux, dict):
        return set()
    selected: set[str] = set()
    for key in ("defconfig", "config_fragment"):
        value = linux.get(key)
        if isinstance(value, str):
            selected.add((base / value).as_posix())
    patches = linux.get("patches")
    if isinstance(patches, list):
        selected.update((base / patch).as_posix() for patch in patches if isinstance(patch, str))
    for key in ("copies", "appends"):
        steps = linux.get(key)
        if isinstance(steps, list):
            selected.update(
                (base / source).as_posix()
                for step in steps
                if isinstance(step, dict) and isinstance((source := step.get("source")), str)
            )
    return selected


def _kernel_scope_paths(snapshot: WorkspaceSnapshot, profile: str | None = None) -> set[str]:
    """Resolve the selected global profile and every board's Linux inputs."""
    profile = normalize_profile(profile)
    by_path = {file.path: file for file in snapshot.files}
    selected = {
        file.path
        for file in snapshot.files
        if file.path in _KERNEL_IMPLEMENTATION
        or file.path == "sources.lock.toml"
        or (
            file.path.startswith(("scripts/fplinux_cli/manifests/", "scripts/fplinux_cli/build/"))
            and file.path.endswith(".py")
        )
    }
    selected.add(f"profiles/{profile or 'default'}/profile.toml")
    target_manifests = [
        file
        for file in snapshot.files
        if (path := PurePath(file.path)).parts[:1] == ("targets",)
        and len(path.parts) == 3
        and path.name == "target.toml"
    ]
    for target_manifest in target_manifests:
        target_path = PurePath(target_manifest.path)
        selected.add(target_manifest.path)
        try:
            target_data = tomllib.loads(target_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        selected.update(_linux_manifest_sources(target_data.get("linux"), base=target_path.parent))
        if profile == "microsd-uboot":
            microsd = target_data.get("microsd", {})
            if isinstance(microsd, dict):
                selected.update(
                    _linux_manifest_sources(
                        {"patches": microsd.get("linux_patches")}, base=target_path.parent
                    )
                )
        platform = target_data.get("platform")
        if not isinstance(platform, str):
            continue
        platform_path = PurePath("platforms") / platform / "platform.toml"
        platform_manifest = by_path.get(platform_path.as_posix())
        if platform_manifest is None:
            continue
        selected.add(platform_manifest.path)
        try:
            platform_data = tomllib.loads(platform_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        selected.update(_linux_manifest_sources(platform_data.get("linux"), base=PurePath()))
    return selected


def check_scope_closure_digest(
    scope: str, snapshot: WorkspaceSnapshot, *, profile: str | None = None
) -> str:
    """Hash only captured files that can affect one exact check scope."""
    if scope == "kernel":
        paths = _kernel_scope_paths(snapshot, profile)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope == "c":
        paths = _c_scope_paths(snapshot)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope in SOURCE_CHECK_SCOPES:
        broaden = (
            scope == "metadata"
            and any(
                Path(file.path).name in _EXECUTABLE_PRETTIER_CONFIGURATION_NAMES
                for file in snapshot.files
            )
        ) or (
            scope == "shell"
            and any(
                file.path == ".shellcheckrc" and b"external-sources=true" in file.contents
                for file in snapshot.files
            )
        )
        selected = [
            file for file in snapshot.files if broaden or _source_scope_uses_file(scope, file)
        ]
    else:
        fail(f"check scope does not support receipts: {scope}")
    if not selected:
        fail(f"check scope has an empty causal closure: {scope}")
    return check_closure_entries_digest(
        [(file.path, file.contents, file.mode) for file in selected]
    )


def check_scope_receipt_recipe(
    scope: str,
    closure_digest: str,
    *,
    image_generation: str,
    orchestration_recipe: str | None = None,
    profile: str | None = None,
) -> CheckReceiptRecipe:
    """Bind one cacheable source scope to its exact closure and OCI identities."""
    if scope not in (*SOURCE_CHECK_SCOPES, "kernel"):
        fail(f"check scope does not support receipts: {scope}")
    if orchestration_recipe is None:
        orchestration_recipe = check_orchestration_recipe_digest()
    return CheckReceiptRecipe(
        scope=scope,
        closure_digest=closure_digest,
        orchestration_recipe=orchestration_recipe,
        image_generation=image_generation,
        profile=profile,
    )


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
    jobs: int,
) -> None:
    """Run cache-missing source and kernel checks against one disposable workspace."""
    container_logs = reporter.root / "containers"
    if container_logs.is_symlink() or (container_logs.exists() and not container_logs.is_dir()):
        fail(f"invalid checker container log directory: {container_logs}")
    container_logs.mkdir(parents=True, exist_ok=True)
    environment = kern_environment()
    log_mount = ["--volume", f"{container_logs}:/logs"]
    source_scopes = [scope for scope in missing if scope in SOURCE_CHECK_SCOPES]
    if source_scopes:
        log_environment = reporter.container_environment("/logs/source")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/containers/source"
        )
        log_arguments = [
            argument
            for key, value in log_environment.items()
            for argument in ("--env", f"{key}={value}")
        ]
        with reporter.stage("source", passthrough=True, show_tail=False) as stage:
            stage.run(
                [
                    kern,
                    "box",
                    kern_box_name("check-source"),
                    "--image",
                    image,
                    "--pull",
                    "never",
                    "--read-only",
                    "--network",
                    "none",
                    "--tmpfs",
                    "/tmp:1g",  # noqa: S108 -- container tmpfs.
                    "--volume",
                    f"{workspace}:/workspace:ro",
                    *log_mount,
                    *log_arguments,
                    "--env",
                    "HOME=/tmp",
                    "--env",
                    "PYTHONPATH=/workspace/scripts",
                    "--env",
                    "RUFF_CACHE_DIR=/tmp/ruff",
                    "--env",
                    "PYTHONDONTWRITEBYTECODE=1",
                    "--workdir",
                    "/workspace",
                    "--init",
                    "--quiet",
                    "--",
                    "python3",
                    "/workspace/scripts/check.py",
                    *source_scopes,
                ],
                env=environment,
                timeout=_SOURCE_CHECK_TIMEOUT,
            )
        for scope in source_scopes:
            publish_success_receipt(cache, recipes[scope])

    if "kernel" not in missing:
        return
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
                "fplinux_cli.kernelcheck",
                "prepare",
                *([] if profile is None else ["--profile", profile]),
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
                "fplinux_cli.kernelcheck",
                "check",
                "--jobs",
                str(jobs),
                *([] if profile is None else ["--profile", profile]),
            ],
            env=environment,
            timeout=_KERNEL_ANALYSIS_TIMEOUT,
        )
    publish_success_receipt(cache, recipes["kernel"])


def check(
    scopes: list[str],
    *,
    verbose: bool = False,
    no_cache: bool = False,
    profile: str | None = None,
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
                check_scope_closure_digest(scope, snapshot, profile=profile),
                image_generation=image_generation,
                orchestration_recipe=orchestration_recipe,
                profile=profile,
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

    image = container_image_reference(container_lock, image_recipe)
    if kern_available(container_lock):
        kern = require_kern(container_lock)
        inspected_image = current_image_state(kern, image, image_recipe)
        if inspected_image is not None:
            current_image = publish_current_image_state(
                kern,
                image,
                image_recipe,
                state=inspected_image,
            )
        else:
            current_image = setup(
                reporter=reporter,
                lock=container_lock,
                image_recipe=image_recipe,
            )
    else:
        current_image = setup(
            reporter=reporter,
            lock=container_lock,
            image_recipe=image_recipe,
        )
        kern = require_kern(container_lock)

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
            jobs=jobs,
        )
    finally:
        discard_staged_quality_workspace_snapshot(snapshot, workspace)
    print("check: OK")
    reporter.finish()
    if profile is not None:
        discard_superseded_profile_logs(cache, "check", profile=profile)
