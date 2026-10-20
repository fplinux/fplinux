# SPDX-License-Identifier: GPL-2.0-only
"""Resolve exact build generations and the checkout's runtime paths."""

from __future__ import annotations

import json
import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.bundles import BundleStateError, resolve_current_bundle
from fplinux_cli.common import fail
from fplinux_cli.device_data.inputs import device_data_cache_directory
from fplinux_cli.manifests.paths import normalize_profile

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.artifacts.bundles import CurrentBundle

MICROSD_BOOT_MODE = "microsd"
MICROSD_BOOT_PROFILE = "microsd-uboot"
PUBLIC_BOOT_MODES = (MICROSD_BOOT_MODE,)


def profile_command(
    command: str, target: str, profile: str | None, *, build_type: str = "release"
) -> str:
    """Render the exact public command for one default or named profile."""
    rendered = f"./fplinux {command} {target} --build-type {build_type}"
    if profile is not None:
        rendered += f" --profile {profile}"
    return rendered


def selected_context_profile(
    target: str | None,
    *,
    profile: str | None,
    boot: str | None,
) -> str | None:
    """Resolve one explicit contributor profile or public boot mode without fallback."""
    if profile is not None and boot is not None:
        fail("--boot and --profile cannot be used together")
    if boot is None:
        return normalize_profile(profile)
    if target is None:
        fail(f"boot mode {boot} requires a target")
    if boot == MICROSD_BOOT_MODE:
        return MICROSD_BOOT_PROFILE
    fail(f"boot mode {boot} is not available for target {target}")
    return None


def bundle_manifest(bundle: CurrentBundle) -> dict[str, Any]:
    """Decode manifest bytes already validated by the immutable bundle resolver."""
    manifest = json.loads(bundle.manifest_bytes)
    if not isinstance(manifest, dict):
        message = "build manifest root must be an object"
        raise BundleStateError(message)
    return manifest


def resolve_target_bundle(
    target: str,
    profile: str | None = None,
    *,
    build_type: str = "release",
) -> tuple[CurrentBundle, dict[str, Any]]:
    """Resolve the current bundle pointer exactly once."""
    try:
        bundle = resolve_current_bundle(
            common.ROOT / ".cache/out",
            target,
            profile,
            build_type=build_type,
        )
        return bundle, bundle_manifest(bundle)
    except (BundleStateError, OSError, UnicodeDecodeError, ValueError) as error:
        fail(
            "current build is missing or invalid; rebuild it: "
            f"{profile_command('build', target, profile, build_type=build_type)} ({error})"
        )


def target_data_root(target: str) -> Path:
    """Use the same personal store for every source-checkout runtime command."""
    return device_data_cache_directory(common.ROOT / ".cache", target)


def bundle_runtime_command(
    bundle: CurrentBundle,
    action: str,
    *arguments: str,
    target: str,
    profile: str | None,
) -> list[str]:
    """Execute the selected bundle's entry point with isolated Python imports."""
    entry = bundle.path / "fplinux.py"
    if entry.is_symlink() or not entry.is_file():
        fail(f"current bundle has no valid runtime entry point: {entry}")
    logs = common.ROOT / ".cache/logs/run" / target
    if profile is not None:
        logs = logs / "profiles" / profile
    return [
        sys.executable,
        "-I",
        str(entry),
        action,
        *arguments,
        "--bundle",
        str(bundle.path),
        "--data-root",
        str(target_data_root(target)),
        "--log-root",
        str(logs),
    ]


def _manifest_device_identity(manifest: dict[str, Any]) -> str:
    """Return the exact kernel-visible identity declared by one build manifest."""
    device_identity = manifest.get("device_identity")
    if (
        not isinstance(device_identity, str)
        or len(device_identity) != 64
        or any(character not in "0123456789abcdef" for character in device_identity)
    ):
        fail("current bundle device identity is invalid")
    return device_identity
