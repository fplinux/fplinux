# SPDX-License-Identifier: GPL-2.0-only
"""Launch the selected immutable bundle's RAM runner."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session

if TYPE_CHECKING:
    from pathlib import Path


def _runnable_target_runner(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
) -> Path:
    """Resolve the fixed shared runner for one runnable bundle."""
    selected_profile = bundle_session.selected_context_profile(target, profile=profile, boot=boot)
    if selected_profile is not None:
        target_config = targets.load_target(target, selected_profile, build_type=build_type)
        if not target_config["runtime"]["runnable"]:
            fail(f"profile is build-only and cannot be run: {target}/{selected_profile}")
    bundle, manifest = bundle_session.resolve_target_bundle(
        target, selected_profile, build_type=build_type
    )
    if selected_profile is not None:
        boot_artifacts = manifest.get("boot_artifacts")
        if not isinstance(boot_artifacts, dict) or boot_artifacts.get("runnable") is not True:
            fail(f"profile bundle is build-only and cannot be run: {target}/{selected_profile}")
    runner = bundle.path / "runner/run.py"
    if runner.is_symlink() or not runner.is_file():
        fail(f"current bundle has no valid runner: {runner}")
    return runner


def run_target(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
    events: Path | None = None,
) -> None:
    """Run the fixed shared runner from a successful target bundle."""
    runner = _runnable_target_runner(target, profile=profile, boot=boot, build_type=build_type)
    argv = [os.fsencode(runner)]
    if events is not None:
        argv.extend([b"--events", os.fsencode(events.resolve())])
    os.execv(os.fsencode(runner), argv)


def run_target_noninteractive(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    events: Path | None = None,
) -> None:
    """Run a loader to its authenticated handoff without taking over this CLI process."""
    runner = _runnable_target_runner(target, profile=profile, build_type=build_type)
    argv = [os.fsencode(runner)]
    if events is not None:
        argv.extend([b"--events", os.fsencode(events.resolve())])
    result = subprocess.run(
        argv,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode:
        fail(f"RAM loader failed with exit status {result.returncode}")
