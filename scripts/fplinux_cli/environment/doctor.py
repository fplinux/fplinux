# SPDX-License-Identifier: GPL-2.0-only
"""Report readiness of the pinned host runtime and prepared build environment."""

from __future__ import annotations

import platform
import subprocess
import sys
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, error_message

from .definition import environment_recipe_digest, load_environment_lock
from .kern import (
    KERN_PROBE_TIMEOUT,
    kern_available,
    kern_box_name,
    kern_environment,
    require_kern,
)
from .runtime import current_environment_state, environment_arguments, environment_rootfs

if TYPE_CHECKING:
    from pathlib import Path


def _kern_build_user_ready(kern: str, rootfs: Path) -> bool:
    """Return whether the host can map the environment's unprivileged package builder."""
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("doctor-build-user"),
                *environment_arguments(rootfs),
                "--uid-range",
                "--read-only",
                "--network",
                "none",
                "--tmpfs",
                "/tmp:16m",  # noqa: S108 -- disposable runtime probe.
                "--quiet",
                "--",
                "sh",
                "-ceu",
                (
                    "install -d -o builder -g builder /tmp/fplinux-builder; "
                    "su builder -s /bin/sh -c 'test -w /tmp/fplinux-builder; "
                    ": > /tmp/fplinux-builder/probe'"
                ),
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False
    return result.returncode == 0


def doctor() -> None:
    problems: list[str] = []
    print(f"host:     {platform.system()} {platform.machine()}")
    if platform.system() != "Linux":
        problems.append("the build interface currently supports Linux hosts only")
    if platform.machine() not in {"x86_64", "amd64"}:
        problems.append("the pinned build environment currently targets linux/amd64")
    lock = load_environment_lock()
    if not kern_available(lock):
        problems.append("Kern is not ready for this checkout; run ./fplinux setup")
    else:
        kern = require_kern(lock)
        try:
            version = subprocess.run(
                [kern, "--version"],
                cwd=ROOT,
                env=kern_environment(),
                capture_output=True,
                text=True,
                check=False,
                timeout=KERN_PROBE_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            problems.append(f"Kern version timed out after {KERN_PROBE_TIMEOUT}s")
        else:
            expected_version = f"kern {lock['kern']['version']}"
            if version.returncode or version.stdout.strip() != expected_version:
                problems.append(version.stderr.strip() or "unexpected Kern version")
            else:
                print(f"kern:      {lock['kern']['version']} (project-local)")
        try:
            runtime = subprocess.run(
                [kern, "doctor"],
                cwd=ROOT,
                env=kern_environment(),
                capture_output=True,
                text=True,
                check=False,
                timeout=KERN_PROBE_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            problems.append(f"Kern doctor timed out after {KERN_PROBE_TIMEOUT}s")
        else:
            if runtime.returncode:
                problems.append(
                    runtime.stderr.strip() or runtime.stdout.strip() or "Kern doctor failed"
                )
            else:
                print("runtime:   ready")
        recipe = environment_recipe_digest(lock)
        state = current_environment_state(kern, recipe)
        readiness = "ready" if state is not None else "not prepared or stale"
        print(f"environment: {readiness}")
        if state is None:
            problems.append("the pinned build environment is not ready; run ./fplinux setup")
        elif not _kern_build_user_ready(kern, environment_rootfs(ROOT / ".cache", state)):
            problems.append(
                "Kern cannot map the package builder; configure newuidmap/newgidmap "
                "and subordinate UID/GID ranges"
            )
    if problems:
        for problem in problems:
            print(error_message(problem), file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")
