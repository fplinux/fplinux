# SPDX-License-Identifier: GPL-2.0-only
"""Select the immutable runtime that owns authenticated NAND operations."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.runtime import bundle_session

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle


def _target_bundle(target: str, profile: str | None, build_type: str) -> CurrentBundle:
    """Select a supported NAND target without starting or preparing its phone."""
    nand = load_target(target, profile=profile, build_type=build_type).get("nand")
    if nand is None:
        fail(f"NAND access is not supported for target {target}")

    bundle, _manifest = bundle_session.resolve_target_bundle(
        target, profile=profile, build_type=build_type
    )
    return bundle


def backup_target_nand(
    target: str,
    output: Path,
    *,
    profile: str | None = None,
    build_type: str = "release",
    phone_data: Path | None = None,
) -> Path:
    """Back up one supported target through its exact current-profile session."""
    bundle = _target_bundle(target, profile, build_type)
    arguments = bundle_session.bundle_runtime_command(
        bundle,
        "nand",
        "backup",
        str(output.resolve()),
        target=target,
        profile=profile,
        phone_data=phone_data,
    )
    result = subprocess.run(arguments, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
    return output


def identify_target_nand(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    phone_data: Path | None = None,
) -> None:
    """Print the chip identity and geometry that the target's running NAND reader reports."""
    bundle = _target_bundle(target, profile, build_type)
    arguments = bundle_session.bundle_runtime_command(
        bundle, "nand", "identify", target=target, profile=profile, phone_data=phone_data
    )
    result = subprocess.run(arguments, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
