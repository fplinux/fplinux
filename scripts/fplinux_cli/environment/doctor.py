# SPDX-License-Identifier: GPL-2.0-only
"""Report the readiness of the pinned host runtime and build image."""

from __future__ import annotations

import platform
import subprocess
import sys

from fplinux_cli.common import ROOT, error_message

from .image_store import current_image_state
from .images import container_image_reference, load_container_lock
from .kern import (
    KERN_PROBE_TIMEOUT,
    kern_available,
    kern_box_name,
    kern_environment,
    require_kern,
)


def _kern_build_user_ready(kern: str, image: str) -> bool:
    """Return whether the host can map the image's unprivileged package builder."""
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("doctor-build-user"),
                "--image",
                image,
                "--pull",
                "never",
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
        problems.append("the pinned build image currently targets linux/amd64")
    lock = load_container_lock()
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
        image = container_image_reference(lock)
        ready = current_image_state(kern, image) is not None
        state = "ready" if ready else "not built or stale"
        print(f"image:     {state} ({image})")
        if not ready:
            problems.append("the pinned build image is not ready; run ./fplinux setup")
        elif not _kern_build_user_ready(kern, image):
            problems.append(
                "Kern cannot map the package builder; configure newuidmap/newgidmap "
                "and subordinate UID/GID ranges"
            )
    if problems:
        for problem in problems:
            print(error_message(problem), file=sys.stderr)
        raise SystemExit(1)
    print("doctor: OK")
