# SPDX-License-Identifier: GPL-2.0-only
"""Manage the single rootless Podman build environment."""

from __future__ import annotations

import platform
import shutil
import subprocess
import sys
from pathlib import Path

from .common import ROOT, fail, run
from .config import container_recipe_digest, load_container_lock
from .output import RunReporter
from .workspace import stage_quality_workspace

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
    "buildroot",
    "c",
    "kernel",
)
SOURCE_CHECK_SCOPES = CHECK_SCOPES[1:-1]
GIT_HOOKS_PATH = ".githooks"


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
    """Return the immutable ID currently assigned to an image tag."""
    if not image_exists(podman, image):
        return None
    result = subprocess.run(
        [podman, "image", "inspect", "--format", "{{.Id}}", image],
        capture_output=True,
        text=True,
        check=False,
    )
    identifier = result.stdout.strip()
    return identifier if result.returncode == 0 and identifier else None


def image_ready(podman: str, image: str) -> bool:
    if not image_exists(podman, image):
        return False
    result = subprocess.run(
        [
            podman,
            "image",
            "inspect",
            "--format",
            '{{ index .Labels "org.fplinux.build.recipe" }}',
            image,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.strip() == container_recipe_digest()


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
    if hooks_path and hooks_path != GIT_HOOKS_PATH:
        fail(f"core.hooksPath is already set to {hooks_path}")
    if not hooks_path:
        subprocess.run(
            [git, "config", "--local", "core.hooksPath", GIT_HOOKS_PATH],
            cwd=ROOT,
            check=True,
        )
    print(f"Git hooks are ready: {GIT_HOOKS_PATH}")


def setup(*, force: bool = False, reporter: RunReporter | None = None) -> None:
    podman = require_podman()
    lock = load_container_lock()
    oci = lock["oci"]
    buildroot = lock["buildroot"]
    if image_ready(podman, oci["image"]) and not force:
        install_git_hooks()
        print(f"Build image is ready: {oci['image']}")
        return
    previous_image = image_identifier(podman, oci["image"])
    command = [
        podman,
        "build",
        "--platform",
        oci["platform"],
        "--tag",
        oci["image"],
        "--file",
        str(ROOT / "Containerfile"),
        "--build-arg",
        f"BASE_IMAGE={oci['base']}",
        "--build-arg",
        f"DEBIAN_SNAPSHOT={oci['debian_snapshot']}",
        "--build-arg",
        f"BUILDROOT_VERSION={buildroot['version']}",
        "--build-arg",
        f"BUILDROOT_URL={buildroot['url']}",
        "--build-arg",
        f"BUILDROOT_SHA256={buildroot['sha256']}",
        "--label",
        f"org.fplinux.build.recipe={container_recipe_digest()}",
        str(ROOT),
    ]
    if reporter is None:
        run(command)
    else:
        with reporter.stage("container-setup") as stage:
            stage.run(command)
    current_image = image_identifier(podman, oci["image"])
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
    if "c" in scopes:
        required.add("analysis")
    if "kernel" in scopes:
        required.update(("analysis", "downloads", "linux"))
    return tuple(name for name in ("analysis", "downloads", "linux") if name in required)


def check(scopes: list[str], *, verbose: bool = False) -> None:
    selected = resolve_check_scopes(scopes)

    reporter = RunReporter.create("check", target=None, verbose=verbose)
    if "repository" in selected:
        check_git_diff(reporter)

    podman = require_podman()
    lock = load_container_lock()["oci"]
    if not image_ready(podman, lock["image"]):
        setup(reporter=reporter)

    cache = ROOT / ".cache"
    analyzer_cache: dict[str, Path] = {}
    for name in analyzer_cache_names(selected):
        source = cache / name
        if source.is_symlink() or (source.exists() and not source.is_dir()):
            fail(f"invalid analyzer cache path: {source}")
        source.mkdir(parents=True, exist_ok=True)
        analyzer_cache[name] = source

    with reporter.stage("workspace"):
        workspace = stage_quality_workspace(enforce_source_policy="source" in selected)

    log_environment = reporter.container_environment("/logs")
    log_arguments = [
        argument
        for key, value in log_environment.items()
        for argument in ("--env", f"{key}={value}")
    ]
    log_mount = ["--volume", f"{reporter.root}:/logs:rw,Z"]
    source_scopes = [scope for scope in selected if scope in SOURCE_CHECK_SCOPES]
    if source_scopes:
        analysis_arguments: list[str] = []
        if "c" in source_scopes:
            scan_recipe = f"{container_recipe_digest()}-{workspace.name}"
            analysis_arguments = [
                "--volume",
                f"{analyzer_cache['analysis']}:/cache/analysis:rw,Z",
                "--env",
                f"FPLINUX_SCAN_BUILD_DIR=/cache/analysis/scan-build/{scan_recipe}",
            ]
        with reporter.stage(
            "source-quality",
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
                    *analysis_arguments,
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
                    lock["image"],
                    "python3",
                    "/workspace/scripts/check.py",
                    *source_scopes,
                ]
            )

    if "kernel" in selected:
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
            lock["image"],
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
                ]
            )

    print("check: OK")
    reporter.finish()
