# SPDX-License-Identifier: GPL-2.0-only
"""Manage the single rootless Podman build environment."""

from __future__ import annotations

import platform
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path, PurePath
from typing import Any

from . import alpine_state
from .checkreceipts import (
    CheckReceiptRecipe,
    check_closure_entries_digest,
    publish_success_receipt,
    receipt_matches,
)
from .common import ROOT, fail
from .config import (
    check_orchestration_recipe_digest,
    container_image_build_arguments,
    container_image_recipe_digest,
    load_container_lock,
)
from .image_state import ImageState, ImageStateError, load_image_state, publish_image_state
from .output import RunReporter
from .workspace import (
    WorkspaceFile,
    WorkspaceSnapshot,
    quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

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
GIT_HOOKS_PATH = ".githooks"
_CONTAINER_FILE = re.compile(r"(?:docker-)?compose(?:\.[^.]+)?\.ya?ml")
_QUOTED_C_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)
_BARE_IMAGE_ID = re.compile(r"[0-9a-f]{64}\Z")
_PREFIXED_IMAGE_ID = re.compile(r"sha256:[0-9a-f]{64}\Z")
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
_SOURCE_CHECK_COMMAND = ("python3", "/workspace/scripts/check.py")
_KERNEL_CHECK_COMMANDS = (
    ("python3", "-m", "fplinux_cli.kernelcheck", "prepare"),
    ("python3", "-m", "fplinux_cli.kernelcheck", "check"),
)
_CHECK_IMPLEMENTATION = frozenset(
    {
        "scripts/check.py",
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/output.py",
    }
)
_ALPINE_CHECK_IMPLEMENTATION = "scripts/fplinux_cli/alpine_state.py"
_KERNEL_IMPLEMENTATION = frozenset(
    {
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/alpine_builder.py",
        "scripts/fplinux_cli/alpine_state.py",
        "scripts/fplinux_cli/builder.py",
        "scripts/fplinux_cli/bundle_state.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/device_state.py",
        "scripts/fplinux_cli/kbuild_state.py",
        "scripts/fplinux_cli/kernelcheck.py",
        "scripts/fplinux_cli/linux_state.py",
        "scripts/fplinux_cli/output.py",
    }
)


def require_podman() -> str:
    executable = shutil.which("podman")
    if executable is None:
        fail("Podman is required (rootless Podman is the supported build backend)")
    return executable


def image_exists(podman: str, image: str) -> bool:
    return (
        subprocess.run(
            [podman, "image", "exists", image],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode
        == 0
    )


def image_identifier(podman: str, image: str) -> str | None:
    """Return the canonical immutable ID currently assigned to an image tag."""
    if not image_exists(podman, image):
        return None
    result = subprocess.run(
        [podman, "image", "inspect", "--format", "{{.Id}}", image],
        capture_output=True,
        text=True,
        check=False,
    )
    identifier = result.stdout.strip()
    if result.returncode != 0:
        return None
    if _BARE_IMAGE_ID.fullmatch(identifier) is not None:
        return f"sha256:{identifier}"
    if _PREFIXED_IMAGE_ID.fullmatch(identifier) is not None:
        return identifier
    return None


def image_ready(
    podman: str,
    image: str,
    *,
    image_recipe: str | None = None,
) -> bool:
    if not image_exists(podman, image):
        return False
    result = subprocess.run(
        [
            podman,
            "image",
            "inspect",
            "--format",
            '{{ index .Labels "org.fplinux.container.image-recipe" }}',
            image,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    return result.returncode == 0 and result.stdout.strip() == image_recipe


def _publish_current_image_state(
    podman: str,
    image: str,
    image_recipe: str,
) -> ImageState:
    """Persist the immutable ID of one image already checked against its recipe."""
    image_identity = image_identifier(podman, image)
    if image_identity is None:
        fail("current build image has no immutable identity")
    try:
        state = ImageState(
            container_image_recipe=image_recipe,
            image_identity=image_identity,
        )
        publish_image_state(ROOT / ".cache", state)
    except ImageStateError as error:
        fail(f"could not publish host image state: {error}")
    return state


def install_git_hooks() -> None:
    """Select the repository-owned hooks for this Git checkout."""
    git = shutil.which("git")
    if git is None:
        return
    checkout = subprocess.run(
        [git, "rev-parse", "--show-toplevel"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if checkout.returncode:
        return
    if Path(checkout.stdout.strip()).resolve() != ROOT:
        fail("Git reports a different repository root")
    configured = subprocess.run(
        [git, "config", "--local", "--get", "core.hooksPath"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if configured.returncode not in {0, 1}:
        fail("could not read the local Git hooks path")
    hooks_path = configured.stdout.strip()
    if hooks_path:
        configured_path = Path(hooks_path)
        if not configured_path.is_absolute():
            configured_path = ROOT / configured_path
        if configured_path.resolve() != (ROOT / GIT_HOOKS_PATH).resolve():
            fail(f"core.hooksPath is already set to {hooks_path}")
    if not hooks_path:
        subprocess.run(
            [git, "config", "--local", "core.hooksPath", GIT_HOOKS_PATH],
            cwd=ROOT,
            check=True,
        )
    print(f"Git hooks are ready: {GIT_HOOKS_PATH}")


def setup(
    *,
    force: bool = False,
    reporter: RunReporter | None = None,
    lock: dict[str, Any] | None = None,
    image_recipe: str | None = None,
) -> ImageState:
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("setup", target=None, verbose=False)
    podman = require_podman()
    if lock is None:
        lock = load_container_lock()
    current_recipe = container_image_recipe_digest(lock)
    if image_recipe is not None and image_recipe != current_recipe:
        fail("container image inputs changed before setup")
    image_recipe = current_recipe
    oci = lock["oci"]
    if image_ready(podman, oci["image"], image_recipe=image_recipe) and not force:
        state = _publish_current_image_state(podman, oci["image"], image_recipe)
        install_git_hooks()
        print(f"Build image is ready: {oci['image']}")
        if own_reporter:
            reporter.finish()
        return state
    previous_image = image_identifier(podman, oci["image"])
    command = [
        podman,
        "build",
        *container_image_build_arguments(lock),
        "--tag",
        oci["image"],
        "--label",
        f"org.fplinux.container.image-recipe={image_recipe}",
        ".",
    ]
    with reporter.stage("container-setup") as stage:
        stage.run(command, cwd=ROOT)
    if container_image_recipe_digest(lock) != image_recipe:
        fail("container image inputs changed while setup was running")
    if not image_ready(podman, oci["image"], image_recipe=image_recipe):
        fail("container setup completed without publishing the exact requested image")
    state = _publish_current_image_state(podman, oci["image"], image_recipe)
    current_image = state.image_identity
    if (
        previous_image is not None
        and current_image is not None
        and current_image != previous_image
    ):
        removed = subprocess.run(
            [podman, "image", "rm", previous_image],
            capture_output=True,
            text=True,
            check=False,
        )
        if removed.returncode:
            detail = removed.stderr.strip() or "image is still in use"
            print(f"warning: replaced build image was retained: {detail}", file=sys.stderr)
        else:
            print(f"Removed replaced build image: {previous_image}")
    install_git_hooks()
    if own_reporter:
        reporter.finish()
    return state


def doctor() -> None:
    problems: list[str] = []
    print(f"host:     {platform.system()} {platform.machine()}")
    if platform.system() != "Linux":
        problems.append("the build interface currently supports Linux hosts only")
    if platform.machine() not in {"x86_64", "amd64"}:
        problems.append("the pinned build image currently targets linux/amd64")
    podman = shutil.which("podman")
    if podman is None:
        problems.append("podman was not found")
    else:
        version = subprocess.run(
            [podman, "version", "--format", "{{.Client.Version}}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if version.returncode:
            problems.append(version.stderr.strip() or "podman version failed")
        else:
            print(f"podman:     {version.stdout.strip()}")
        rootless = subprocess.run(
            [podman, "info", "--format", "{{.Host.Security.Rootless}}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if rootless.returncode or rootless.stdout.strip() != "true":
            problems.append("rootless Podman is not active")
        else:
            print("rootless:   yes")
        image = load_container_lock()["oci"]["image"]
        state = "ready" if image_ready(podman, image) else "not built or stale"
        print(f"image:      {state} ({image})")
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")


def check_git_diff(reporter: RunReporter) -> None:
    head = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if head.returncode == 0:
        with reporter.stage("git-diff") as stage:
            stage.run(["git", "diff", "--check", "HEAD", "--"], cwd=ROOT)


def check_commit_message(message_file: str) -> None:
    """Validate one Git commit message in the pinned environment."""
    message = Path(message_file)
    if message.is_symlink() or not message.is_file():
        fail(f"commit message file is missing or invalid: {message}")
    config = ROOT / "commitlint.config.mjs"
    if config.is_symlink() or not config.is_file():
        fail("commitlint configuration is missing or invalid")
    podman = require_podman()
    lock = load_container_lock()["oci"]
    if not image_ready(podman, lock["image"]):
        fail("commit hook requires the current build image; run ./fplinux setup")
    result = subprocess.run(
        [
            podman,
            "run",
            "--rm",
            "--platform",
            lock["platform"],
            "--userns=keep-id",
            "--network=none",
            "--read-only",
            "--tmpfs",
            "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs.
            "--volume",
            f"{config}:/workspace/commitlint.config.mjs:ro,Z",
            "--volume",
            f"{message.resolve()}:/message:ro,Z",
            "--env",
            "HOME=/tmp",
            "--workdir",
            "/workspace",
            lock["image"],
            "sh",
            "-c",
            "commitlint < /message",
        ],
        cwd=ROOT,
        check=False,
    )
    if result.returncode:
        raise SystemExit(result.returncode)


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
    if Path(file.path).suffix not in {"", ".initd", ".sh"}:
        return False
    first_line = file.contents.splitlines()[:1]
    if not first_line:
        return False
    try:
        shebang = first_line[0].decode().strip()
    except UnicodeDecodeError:
        return False
    return shebang in {
        "#!/bin/sh",
        "#!/usr/bin/env sh",
        "#!/usr/bin/env bash",
        "#!/sbin/openrc-run",
    }


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
    suffix = path.suffix.lower()
    parts = path.parts
    if file.path in _CHECK_IMPLEMENTATION or (
        scope in {"alpine", "c"} and file.path == _ALPINE_CHECK_IMPLEMENTATION
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
        return (
            name in {"Containerfile", "Dockerfile"}
            or _CONTAINER_FILE.fullmatch(name) is not None
            or name.startswith(".hadolint")
        )
    if scope == "metadata":
        return (
            suffix == ".toml"
            or (suffix in {".json", ".jsonc"} and name != "package-lock.json")
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
        except (UnicodeDecodeError, tomllib.TOMLDecodeError):
            continue
        tools = manifest.get("host", {}).get("tools", [])
        if not isinstance(tools, list):
            continue
        for recipe in tools:
            if isinstance(recipe, dict) and recipe.get("type") == "cc-libusb/v1":
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
    defconfig = linux.get("defconfig")
    if isinstance(defconfig, str):
        selected.add((base / defconfig).as_posix())
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


def _kernel_scope_paths(snapshot: WorkspaceSnapshot) -> set[str]:
    """Resolve exact Linux inputs named by the captured target manifests."""
    by_path = {file.path: file for file in snapshot.files}
    selected = {
        file.path
        for file in snapshot.files
        if file.path in _KERNEL_IMPLEMENTATION or file.path == "sources.lock.toml"
    }
    target_manifests = (
        file
        for file in snapshot.files
        if (path := PurePath(file.path)).parts[:1] == ("targets",)
        and len(path.parts) == 3
        and path.name == "target.toml"
    )
    for target_manifest in target_manifests:
        target_path = PurePath(target_manifest.path)
        selected.add(target_manifest.path)
        try:
            target = tomllib.loads(target_manifest.contents.decode("utf-8"))
        except (UnicodeDecodeError, tomllib.TOMLDecodeError):
            continue
        if not isinstance(target, dict):
            continue
        selected.update(_linux_manifest_sources(target.get("linux"), base=target_path.parent))
        platform = target.get("platform")
        if not isinstance(platform, str):
            continue
        platform_path = PurePath("platforms") / platform / "platform.toml"
        platform_manifest = by_path.get(platform_path.as_posix())
        if platform_manifest is None:
            continue
        selected.add(platform_manifest.path)
        try:
            platform_data = tomllib.loads(platform_manifest.contents.decode("utf-8"))
        except (UnicodeDecodeError, tomllib.TOMLDecodeError):
            continue
        if isinstance(platform_data, dict):
            selected.update(_linux_manifest_sources(platform_data.get("linux"), base=PurePath()))
    return selected


def check_scope_closure_digest(scope: str, snapshot: WorkspaceSnapshot) -> str:
    """Hash only captured files that can affect one exact check scope."""
    if scope == "kernel":
        paths = _kernel_scope_paths(snapshot)
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


def check_scope_commands(scope: str) -> tuple[tuple[str, ...], ...]:
    """Return the canonical checker argv whose result a receipt certifies."""
    if scope in SOURCE_CHECK_SCOPES:
        return ((*_SOURCE_CHECK_COMMAND, scope),)
    if scope == "kernel":
        return _KERNEL_CHECK_COMMANDS
    fail(f"check scope does not support receipts: {scope}")
    return ()


def check_scope_receipt_recipe(
    scope: str,
    closure_digest: str,
    *,
    image_identity: str,
    commands: tuple[tuple[str, ...], ...] | None = None,
    orchestration_recipe: str | None = None,
) -> CheckReceiptRecipe:
    """Bind one cacheable source scope to its exact closure and OCI identities."""
    if scope not in (*SOURCE_CHECK_SCOPES, "kernel"):
        fail(f"check scope does not support receipts: {scope}")
    if commands is None:
        commands = check_scope_commands(scope)
    if orchestration_recipe is None:
        orchestration_recipe = check_orchestration_recipe_digest()
    return CheckReceiptRecipe(
        scope=scope,
        closure_digest=closure_digest,
        orchestration_recipe=orchestration_recipe,
        image_identity=image_identity,
        commands=commands,
    )


def check(
    scopes: list[str],
    *,
    verbose: bool = False,
    no_cache: bool = False,
) -> None:
    selected = resolve_check_scopes(scopes)

    reporter = RunReporter.create("check", target=None, verbose=verbose)
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

    def receipt_recipes(image_identity: str) -> dict[str, CheckReceiptRecipe]:
        return {
            scope: check_scope_receipt_recipe(
                scope,
                check_scope_closure_digest(scope, snapshot),
                image_identity=image_identity,
                orchestration_recipe=orchestration_recipe,
            )
            for scope in cacheable_scopes
        }

    cached_image = load_image_state(cache, image_recipe)
    if cached_image is not None:
        recipes = receipt_recipes(cached_image.image_identity)
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

    podman = require_podman()
    lock = container_lock["oci"]
    if image_ready(podman, lock["image"], image_recipe=image_recipe):
        current_image = _publish_current_image_state(
            podman,
            lock["image"],
            image_recipe,
        )
    else:
        current_image = setup(
            reporter=reporter,
            lock=container_lock,
            image_recipe=image_recipe,
        )

    image_identity = current_image.image_identity
    recipes = receipt_recipes(current_image.image_identity)
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

    log_mount = ["--volume", f"{reporter.root}:/logs:rw,Z"]
    source_scopes = [scope for scope in missing if scope in SOURCE_CHECK_SCOPES]
    if source_scopes:
        log_environment = reporter.container_environment("/logs/source")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/source"
        )
        log_arguments = [
            argument
            for key, value in log_environment.items()
            for argument in ("--env", f"{key}={value}")
        ]
        with reporter.stage(
            "source",
            passthrough=True,
            show_tail=False,
        ) as stage:
            stage.run(
                [
                    podman,
                    "run",
                    "--rm",
                    "--platform",
                    lock["platform"],
                    "--userns=keep-id",
                    "--network=none",
                    "--read-only",
                    "--tmpfs",
                    "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs.
                    "--volume",
                    f"{workspace}:/workspace:ro,Z",
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
                    image_identity,
                    "python3",
                    "/workspace/scripts/check.py",
                    *source_scopes,
                ]
            )
        for scope in source_scopes:
            publish_success_receipt(cache, recipes[scope])

    if "kernel" in missing:
        log_environment = reporter.container_environment("/logs/kernel")
        log_environment["FPLINUX_LOG_DISPLAY_ROOT"] = (
            f"{log_environment['FPLINUX_LOG_DISPLAY_ROOT']}/kernel"
        )
        log_arguments = [
            argument
            for key, value in log_environment.items()
            for argument in ("--env", f"{key}={value}")
        ]
        analyzer_runtime = [
            podman,
            "run",
            "--rm",
            "--platform",
            lock["platform"],
            "--userns=keep-id",
        ]
        analyzer_program = [
            "--read-only",
            "--tmpfs",
            "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs.
            "--volume",
            f"{workspace}:/workspace:ro,Z",
            *log_mount,
            *log_arguments,
            "--env",
            "HOME=/tmp",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            "PYTHONDONTWRITEBYTECODE=1",
            image_identity,
            "python3",
            "-m",
            "fplinux_cli.kernelcheck",
        ]
        with reporter.stage(
            "kernel-prepare",
            passthrough=True,
            show_tail=False,
        ) as stage:
            stage.run(
                [
                    *analyzer_runtime,
                    "--volume",
                    f"{analyzer_cache['downloads']}:/cache/downloads:rw,Z",
                    "--volume",
                    f"{analyzer_cache['linux']}:/cache/linux:rw,Z",
                    *analyzer_program,
                    "prepare",
                ]
            )
        with reporter.stage(
            "kernel-analysis",
            passthrough=True,
            show_tail=False,
        ) as stage:
            stage.run(
                [
                    *analyzer_runtime,
                    "--network=none",
                    "--volume",
                    f"{analyzer_cache['analysis']}:/cache/analysis:rw,Z",
                    "--volume",
                    f"{analyzer_cache['linux']}:/cache/linux:ro,Z",
                    *analyzer_program,
                    "check",
                ],
            )
        publish_success_receipt(cache, recipes["kernel"])

    print("check: OK")
    reporter.finish()
