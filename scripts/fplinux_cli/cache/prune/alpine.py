# SPDX-License-Identifier: GPL-2.0-only
"""Retain Alpine package and rootfs caches selected by current targets."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.alpine.lock import PACKAGE_ID
from fplinux_cli.alpine.packages import PACKAGE_CACHE_DIRECTORY
from fplinux_cli.alpine.recipes import alpine_rootfs_recipe
from fplinux_cli.alpine.selection import aport_producer, bundle_packages, selected_packages
from fplinux_cli.alpine.signing import signing_key_identity
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.environment.image_state import load_image_state
from fplinux_cli.environment.images import (
    container_artifact_recipe_digest,
    container_image_recipe_digest,
)
from fplinux_cli.manifests.paths import discover_profiles, discover_targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from .model import InventoryEntry
from .paths import tree_size

if TYPE_CHECKING:
    from pathlib import Path


def _current_rootfs_recipes(cache: Path) -> frozenset[str] | None:
    """Return every currently valid rootfs recipe, or ``None`` if that is unknown."""
    try:
        signing_key = signing_key_identity(cache)
        image_source_recipe = container_image_recipe_digest()
        image_state = load_image_state(cache, image_source_recipe)
        if image_state is None:
            return None
        image_recipe = container_artifact_recipe_digest(
            image_source_recipe,
            image_state.image_content,
        )
        recipes: set[str] = set()
        for target in discover_targets():
            for profile in (None, *discover_profiles(target)):
                target_config = (
                    load_target(target) if profile is None else load_target(target, profile)
                )
                device_data_groups = target_config["device_data"]["groups"]
                device_data = firmware_inputs.capture_external_device_data(
                    target,
                    device_data_groups,
                    cache,
                )
                firmware = firmware_inputs.rootfs_firmware_inputs(device_data)
                recipes.add(
                    alpine_rootfs_recipe(
                        image_recipe,
                        signing_key,
                        selected_packages(load_platform(target_config["platform"]), target_config),
                        firmware_inputs=firmware,
                        display_brightness=target_config.get("display_brightness"),
                        root_kind=target_config["linux"]["root"]["kind"],
                    )
                )
    except OSError, ValueError, SystemExit:
        return None
    return frozenset(recipes)


def rootfs_entries(cache: Path) -> list[InventoryEntry]:
    """Classify immutable Alpine rootfs generations by all current target recipes."""
    rootfs = cache / "rootfs"
    if rootfs.is_symlink():
        return [
            InventoryEntry(
                "rootfs",
                "protected",
                "rootfs cache root is a symlink",
                None,
                None,
            )
        ]
    if not rootfs.is_dir():
        return []
    current = _current_rootfs_recipes(cache)
    entries: list[InventoryEntry] = []
    for path in sorted(rootfs.iterdir(), key=lambda item: item.name):
        identity = f"rootfs/{path.name}"
        if not path.is_dir() or path.is_symlink():
            entries.append(
                InventoryEntry(identity, "protected", "not a rootfs directory", None, None)
            )
        elif current is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current rootfs recipes are unavailable",
                    None,
                    None,
                )
            )
        elif path.name in current:
            entries.append(
                InventoryEntry(identity, "protected", "current Alpine rootfs", None, None)
            )
        else:
            logical, allocated = tree_size(path)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate",
                    "superseded Alpine rootfs",
                    logical,
                    allocated,
                )
            )
    return entries


def _current_apk_packages() -> frozenset[str] | None:
    """Return every current aport name, or ``None`` when declarations are unavailable."""
    try:
        packages: set[str] = set()
        for target in discover_targets():
            for profile in (None, *discover_profiles(target)):
                target_config = (
                    load_target(target) if profile is None else load_target(target, profile)
                )
                platform_config = load_platform(target_config["platform"])
                rootfs_packages = selected_packages(
                    platform_config,
                    target_config,
                )
                packages.update(rootfs_packages)
                packages.update(
                    bundle_packages(
                        platform_config,
                        target_config,
                        rootfs_packages,
                    )
                )
    except KeyError, OSError, TypeError, ValueError, SystemExit:
        return None
    return frozenset(aport_producer(package) for package in packages)


def apk_entries(cache: Path) -> list[InventoryEntry]:
    """Classify one fixed cache slot for every currently declared aport."""
    apks = cache / PACKAGE_CACHE_DIRECTORY
    if apks.is_symlink():
        return [
            InventoryEntry(
                PACKAGE_CACHE_DIRECTORY,
                "protected",
                "APK cache root is a symlink",
                None,
                None,
            )
        ]
    if not apks.is_dir():
        return []
    current = _current_apk_packages()
    entries: list[InventoryEntry] = []
    for path in sorted(apks.iterdir(), key=lambda item: item.name):
        identity = f"{PACKAGE_CACHE_DIRECTORY}/{path.name}"
        if path.is_symlink() or not path.is_dir() or PACKAGE_ID.fullmatch(path.name) is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "not a managed APK cache directory",
                    None,
                    None,
                )
            )
        elif current is None:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current Alpine package closure is unavailable",
                    None,
                    None,
                )
            )
        elif path.name in current:
            entries.append(
                InventoryEntry(
                    identity,
                    "protected",
                    "current Alpine package cache",
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
                    "superseded Alpine package cache",
                    logical,
                    allocated,
                )
            )
    return entries
