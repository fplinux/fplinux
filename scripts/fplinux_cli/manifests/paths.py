# SPDX-License-Identifier: GPL-2.0-only
"""Discover declared targets, platforms and global boot profiles."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.values import TARGET_NAME

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path

GLOBAL_PROFILES = ("default", "microsd-uboot", "microsd-root")


def normalize_profile(profile: str | None) -> str | None:
    """Keep explicit default and omitted selection in the same build context."""
    if profile is None or profile == "default":
        return None
    if profile not in GLOBAL_PROFILES:
        fail(f"unknown profile: {profile}; choose from {', '.join(GLOBAL_PROFILES)}")
    return profile


def _manifest_directories(directory: str, manifest: str) -> tuple[str, ...]:
    """Return the validly named subdirectories that hold one regular manifest file."""
    root = common.ROOT / directory
    return tuple(
        path.name
        for path in sorted(root.iterdir())
        if path.is_dir()
        and not path.is_symlink()
        and TARGET_NAME.fullmatch(path.name) is not None
        and (path / manifest).is_file()
        and not (path / manifest).is_symlink()
    )


def discover_targets() -> tuple[str, ...]:
    """Discover data-only target manifests without a central registry."""
    targets = _manifest_directories("targets", "target.toml")
    if not targets:
        fail("no targets are defined")
    return targets


def discover_platforms() -> tuple[str, ...]:
    """Discover platform manifests without a central registry."""
    platforms = _manifest_directories("platforms", "platform.toml")
    if not platforms:
        fail("no platforms are defined")
    return platforms


def target_directory(target: str) -> Path:
    """Return one validated target directory in the fixed repository layout."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    path = common.ROOT / "targets" / target
    if path.is_symlink() or not path.is_dir():
        fail(f"unknown target: {target}")
    return path


def target_release_manifest_path(target: str) -> Path:
    """Return the fixed release-manifest path for one target."""
    return target_directory(target) / "release/manifest.toml"


def platform_asset_lock_path(platform: str) -> Path:
    """Return the fixed loader asset-lock path for one platform."""
    if TARGET_NAME.fullmatch(platform) is None:
        fail(f"invalid platform name: {platform}")
    path = common.ROOT / "platforms" / platform
    if path.is_symlink() or not path.is_dir():
        fail(f"unknown platform: {platform}")
    return path / "loader/assets.lock.toml"


def profiles_directory(target: str | None = None) -> Path:
    """Return the shared build-profile directory."""
    if target is not None:
        target_directory(target)
    path = common.ROOT / "profiles"
    if path.is_symlink() or not path.is_dir():
        fail(f"profiles directory is missing or invalid: {path}")
    return path


def profile_directory(target: str, profile: str) -> Path:
    """Return one declared global profile directory."""
    normalize_profile(profile)
    path = profiles_directory(target) / profile
    if path.is_symlink() or not path.is_dir():
        fail(f"global profile directory is missing or invalid: {path}")
    return path


def profile_manifest_path(target: str, profile: str) -> Path:
    """Return the shared manifest for the selected build profile."""
    path = profile_directory(target, profile) / "profile.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"profile manifest is missing or invalid: {path}")
    return path


def declared_boot_profiles(target: Mapping[str, object]) -> tuple[str, ...]:
    """Select current boot policies from the board's platform and microSD declaration."""
    if "microsd" not in target:
        return ("default",)
    if target.get("platform") == "sc9820e":
        return ("default", "microsd-root")
    return ("default", "microsd-uboot")


def discover_profiles(target: str) -> tuple[str, ...]:
    """Expose the policies supported by the selected board's boot integration."""
    profiles = declared_boot_profiles(load_toml(target_directory(target) / "target.toml"))
    for profile in profiles:
        profile_manifest_path(target, profile)
    return profiles
