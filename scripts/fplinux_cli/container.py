# SPDX-License-Identifier: GPL-2.0-only
"""Manage the single rootless Podman build environment."""

from __future__ import annotations

import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from .common import ROOT, fail, run
from .config import load_toolchain, toolchain_recipe_digest
from .workspace import stage_quality_workspace


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
    return result.returncode == 0 and result.stdout.strip() == toolchain_recipe_digest()


def setup(*, force: bool = False) -> None:
    podman = require_podman()
    lock = load_toolchain()
    oci = lock["oci"]
    buildroot = lock["buildroot"]
    if image_ready(podman, oci["image"]) and not force:
        print(f"Build image is ready: {oci['image']}")
        return
    previous_image = image_identifier(podman, oci["image"])
    run(
        [
            podman,
            "build",
            "--platform",
            oci["platform"],
            "--tag",
            oci["image"],
            "--file",
            str(ROOT / "toolchains/Containerfile"),
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
            f"org.fplinux.build.recipe={toolchain_recipe_digest()}",
            str(ROOT),
        ]
    )
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
        image = load_toolchain()["oci"]["image"]
        state = "ready" if image_ready(podman, image) else "not built or stale"
        print(f"image:      {state} ({image})")
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")


def check_git_diff() -> None:
    head = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if head.returncode == 0:
        print("+ git diff --check HEAD --", flush=True)
        subprocess.run(["git", "diff", "--check", "HEAD", "--"], cwd=ROOT, check=True)


def check_commit_messages(podman: str, lock: dict[str, str], workspace: Path) -> None:
    """Validate every commit message against the conventional convention."""
    head = subprocess.run(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if head.returncode:
        print("commitlint: no commits to check")
        return
    revisions = subprocess.run(
        ["git", "log", "--format=%H"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    with tempfile.TemporaryDirectory(prefix="fplinux-commitlint-") as messages:
        for index, revision in enumerate(revisions):
            message = subprocess.run(
                ["git", "log", "-1", "--format=%B", revision],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            (Path(messages) / f"{index:05d}.message").write_text(message)
        run(
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
                "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs, not a host path.
                "--volume",
                f"{workspace}:/workspace:ro,Z",
                "--volume",
                f"{messages}:/messages:ro,Z",
                "--env",
                "HOME=/tmp",
                "--workdir",
                "/workspace",
                lock["image"],
                "sh",
                "-c",
                'for message in /messages/*.message; do commitlint < "${message}" || exit 1; done',
            ]
        )
    print(f"commitlint: OK ({len(revisions)} commit messages)")


def check() -> None:
    check_git_diff()
    podman = require_podman()
    lock = load_toolchain()["oci"]
    if not image_ready(podman, lock["image"]):
        setup()

    cache = ROOT / ".cache"
    analyzer_cache = {}
    for name in ("analysis", "downloads", "linux"):
        source = cache / name
        if source.is_symlink() or (source.exists() and not source.is_dir()):
            fail(f"invalid analyzer cache path: {source}")
        source.mkdir(parents=True, exist_ok=True)
        analyzer_cache[name] = source

    workspace = stage_quality_workspace()
    check_commit_messages(podman, lock, workspace)
    scan_recipe = f"{toolchain_recipe_digest()}-{workspace.name}"
    run(
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
            "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs, not a host path.
            "--volume",
            f"{workspace}:/workspace:ro,Z",
            "--volume",
            f"{analyzer_cache['analysis']}:/cache/analysis:rw,Z",
            "--env",
            f"FPLINUX_SCAN_BUILD_DIR=/cache/analysis/scan-build/{scan_recipe}",
            "--env",
            "HOME=/tmp",
            "--env",
            "RUFF_CACHE_DIR=/tmp/ruff",
            "--env",
            "PYTHONDONTWRITEBYTECODE=1",
            lock["image"],
            "python3",
            "/workspace/scripts/check.py",
        ]
    )

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
        "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs, not a host path.
        "--volume",
        f"{workspace}:/workspace:ro,Z",
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
    run(
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
    run(
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
