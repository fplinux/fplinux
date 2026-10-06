# SPDX-License-Identifier: GPL-2.0-only
"""Retain declared profile slots without touching default target state."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.manifests.paths import discover_profiles, discover_targets
from fplinux_cli.manifests.values import TARGET_NAME

from .model import InventoryEntry
from .paths import tree_size

if TYPE_CHECKING:
    from pathlib import Path


def declared_profiles() -> dict[str, frozenset[str]] | None:
    """Return all target-owned profile names, or preserve cache if discovery fails."""
    try:
        return {target: frozenset(discover_profiles(target)) for target in discover_targets()}
    except OSError, ValueError, SystemExit:
        return None


def _profile_slot_entries(
    root: Path,
    identity_root: str,
    declared: dict[str, frozenset[str]] | None,
) -> list[InventoryEntry]:
    """Classify managed target/profile slots without touching default target state."""
    if not root.is_dir() or root.is_symlink():
        return []
    entries: list[InventoryEntry] = []
    for target in sorted(root.iterdir(), key=lambda item: item.name):
        if (
            target.is_symlink()
            or not target.is_dir()
            or TARGET_NAME.fullmatch(target.name) is None
        ):
            continue
        profiles = target / "profiles"
        if not profiles.is_dir() or profiles.is_symlink():
            continue
        for path in sorted(profiles.iterdir(), key=lambda item: item.name):
            identity = f"{identity_root}/{target.name}/profiles/{path.name}"
            if path.is_symlink() or not path.is_dir() or TARGET_NAME.fullmatch(path.name) is None:
                entries.append(
                    InventoryEntry(
                        identity,
                        "protected",
                        "not a managed profile cache directory",
                        None,
                        None,
                    )
                )
            elif declared is None:
                entries.append(
                    InventoryEntry(
                        identity,
                        "protected",
                        "profile declarations are unavailable",
                        None,
                        None,
                    )
                )
            elif path.name in declared.get(target.name, frozenset()):
                entries.append(
                    InventoryEntry(
                        identity,
                        "protected",
                        "declared profile cache",
                        None,
                        None,
                    )
                )
            else:
                logical, allocated = tree_size(path)
                entries.append(
                    InventoryEntry(
                        identity,
                        "candidate",
                        "orphaned managed profile cache",
                        logical,
                        allocated,
                    )
                )
    return entries


def profile_cache_entries(cache: Path) -> list[InventoryEntry]:
    """Remove only orphaned profile-only cache slots in explicit CLI namespaces."""
    declared = declared_profiles()
    return [
        *_profile_slot_entries(cache / "out", "out", declared),
        *_profile_slot_entries(cache / "analysis" / "sparse", "analysis/sparse", declared),
    ]


def profile_check_receipt_entries(cache: Path) -> list[InventoryEntry]:
    """Classify fixed profile check receipts removed from every target declaration."""
    root = cache / "check-results" / "profiles"
    if not root.is_dir() or root.is_symlink():
        return []
    declared = declared_profiles()
    entries: list[InventoryEntry] = []
    for profile in sorted(root.iterdir(), key=lambda item: item.name):
        identity = f"check-results/profiles/{profile.name}"
        if (
            profile.is_symlink()
            or not profile.is_dir()
            or TARGET_NAME.fullmatch(profile.name) is None
        ):
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "not a managed profile receipt directory",
                    None,
                    None,
                )
            )
        elif declared is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "profile declarations are unavailable",
                    None,
                    None,
                )
            )
        elif any(profile.name in profiles for profiles in declared.values()):
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "declared profile check receipt",
                    None,
                    None,
                )
            )
        else:
            logical, allocated = tree_size(profile)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "orphaned managed profile check receipt",
                    logical,
                    allocated,
                )
            )
    return entries
