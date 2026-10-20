# SPDX-License-Identifier: GPL-2.0-only
"""Select an immutable runtime and pass it the checkout's state paths."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.manifests import targets
from fplinux_cli.runtime import bundle_session

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle


def _runnable_target_bundle(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
) -> tuple[CurrentBundle, str | None]:
    """Resolve one runnable generation without rebuilding or changing its runtime."""
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
    return bundle, selected_profile


def run_target(  # noqa: PLR0913 -- build selectors and runtime inputs are independent.
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
    from_dump: Path | None = None,
    events: Path | None = None,
    loader_trace: bool = False,
) -> None:
    """Replace the selector with the selected generation's isolated runtime."""
    bundle, selected_profile = _runnable_target_bundle(
        target, profile=profile, boot=boot, build_type=build_type
    )
    arguments = bundle_session.bundle_runtime_command(
        bundle, "run", target=target, profile=selected_profile
    )
    if from_dump is not None:
        arguments.extend(["--from-dump", str(from_dump.resolve())])
    if events is not None:
        arguments.extend(["--events", str(events.resolve())])
    if loader_trace:
        arguments.append("--loader-trace")
    os.execv(sys.executable, arguments)


def prepare_target(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    replace: bool = False,
    events: Path | None = None,
) -> None:
    """Run the selected bundle's complete reader and preparation workflow."""
    bundle, selected_profile = _runnable_target_bundle(
        target, profile=profile, build_type=build_type
    )
    arguments = bundle_session.bundle_runtime_command(
        bundle, "prepare", target=target, profile=selected_profile
    )
    if replace:
        arguments.append("--replace")
    if events is not None:
        arguments.extend(["--events", str(events.resolve())])
    result = subprocess.run(arguments, stdin=subprocess.DEVNULL, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)


def reconnect_bundle(  # noqa: PLR0913 -- each remote action is an existing CLI mode.
    bundle: CurrentBundle,
    target: str,
    *,
    profile: str | None,
    exec_command: str | None = None,
    upload: list[str] | None = None,
    pull: list[str] | None = None,
) -> None:
    """Use the selected runtime's authenticated session without allowing a fresh boot."""
    arguments = bundle_session.bundle_runtime_command(
        bundle, "run", target=target, profile=profile
    )
    arguments.append("--reconnect")
    if exec_command is not None:
        arguments.extend(["--exec", exec_command])
    elif upload is not None:
        arguments.extend(["--upload", *upload])
    elif pull is not None:
        arguments.extend(["--pull", *pull])
    result = subprocess.run(arguments, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
