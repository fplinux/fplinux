# SPDX-License-Identifier: GPL-2.0-only
"""Discover declared targets, platforms and global boot profiles."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.values import TARGET_NAME

if TYPE_CHECKING:
    from pathlib import Path

GLOBAL_PROFILES = ("default", "microsd-uboot")


def normalize_profile(profile: str | None) -> str | None:
    """Keep explicit default and omitted selection in the same build context."""
    if profile is None or profile == "default":
        return None
    if profile not in GLOBAL_PROFILES:
        fail(f"unknown profile: {profile}; choose default or microsd-uboot")
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


def target_asset_lock_path(target: str) -> Path:
    """Return the fixed loader asset-lock path for one target."""
    return target_directory(target) / "loader/assets.lock.toml"


def profiles_directory(target: str | None = None) -> Path:
    """Return the shared build-profile directory."""
    if target is not None:
        target_directory(target)
    path = common.ROOT / "profiles"
    if path.is_symlink() or not path.is_dir():
        fail(f"profiles directory is missing or invalid: {path}")
    return path


def profile_directory(target: str, profile: str) -> Path:
    """Return one of the two global profile directories."""
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


def discover_profiles(target: str) -> tuple[str, ...]:
    """Expose both boot policies, or only the default one for a target without microSD inputs."""
    for profile in GLOBAL_PROFILES:
        profile_manifest_path(target, profile)
    if "microsd" not in load_toml(target_directory(target) / "target.toml"):
        return ("default",)
    return GLOBAL_PROFILES
