# SPDX-License-Identifier: GPL-2.0-only
"""Retain Alpine package and rootfs caches selected by current targets."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.alpine.lock import PACKAGE_ID
from fplinux_cli.alpine.packages import PACKAGE_CACHE_DIRECTORY
from fplinux_cli.alpine.recipes import alpine_rootfs_recipe
from fplinux_cli.alpine.rootfs import shared_card_packages, shared_ram_packages
from fplinux_cli.alpine.rootfs_state import layered_rootfs_recipe
from fplinux_cli.alpine.selection import (
    aport_build_order,
    bundle_packages,
    selected_applications,
    selected_base_packages,
    selected_packages,
)
from fplinux_cli.alpine.signing import signing_key_identity
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.environment.definition import (
    environment_artifact_recipe_digest,
    environment_recipe_digest,
)
from fplinux_cli.environment.state import load_environment_state
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.paths import discover_profiles, discover_targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from .model import InventoryEntry
from .paths import tree_size

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path
    from typing import Any


def _current_targets() -> Iterator[tuple[str, dict[str, Any], dict[str, Any]]]:
    """Load each currently supported target, profile and build type."""
    for target in discover_targets():
        for profile in discover_profiles(target):
            for build_type in BUILD_TYPES:
                config = load_target(target, profile, build_type=build_type)
                yield target, config, load_platform(config["platform"])


def _current_rootfs_recipes(cache: Path) -> frozenset[str] | None:
    """Return every currently valid rootfs recipe, or ``None`` if that is unknown."""
    try:
        signing_key = signing_key_identity(cache)
        release = current_release_context()
        environment_source_recipe = environment_recipe_digest()
        environment_state = load_environment_state(cache, environment_source_recipe)
        if environment_state is None:
            return None
        environment_recipe = environment_artifact_recipe_digest(
            environment_source_recipe,
            environment_state.content,
        )
        recipes: set[str] = set()
        for target, target_config, platform in _current_targets():
            device_data_groups = target_config["device_data"]["groups"]
            device_data = firmware_inputs.capture_external_device_data(
                target,
                device_data_groups,
            )
            root_kind = target_config["linux"]["root"]["kind"]
            card_profile = target_config["profile"] if root_kind == "external" else None
            firmware = (
                firmware_inputs.rootfs_firmware_inputs(device_data)
                if firmware_inputs.rootfs_receives_firmware(root_kind=root_kind)
                else ()
            )
            packages = selected_base_packages(platform, target_config)
            applications = selected_applications(target_config)
            ram_base_recipe = None
            base_packages = shared_ram_packages(platform, target_config)
            if root_kind == "initramfs" and base_packages is not None:
                ram_base_recipe = alpine_rootfs_recipe(
                    environment_recipe,
                    signing_key,
                    base_packages,
                    release=release,
                    applications=applications,
                )
                recipes.add(ram_base_recipe)
            card_base_packages = shared_card_packages(platform, target_config)
            if card_base_packages is not None:
                recipes.add(
                    alpine_rootfs_recipe(
                        environment_recipe,
                        signing_key,
                        card_base_packages,
                        release=release,
                        root_kind="external",
                        card_profile=card_profile,
                    )
                )
                recipes.add(
                    alpine_rootfs_recipe(
                        environment_recipe,
                        signing_key,
                        packages,
                        release=release,
                        display_brightness=target_config.get("display_brightness"),
                        root_kind="external",
                        card_profile=card_profile,
                    )
                )
            firmware_selections = (firmware, ()) if firmware else ((),)
            for selected_firmware in firmware_selections:
                recipe = alpine_rootfs_recipe(
                    environment_recipe,
                    signing_key,
                    packages,
                    release=release,
                    applications=applications,
                    firmware_inputs=selected_firmware,
                    display_brightness=target_config.get("display_brightness"),
                    root_kind=root_kind,
                    card_profile=card_profile,
                )
                recipes.add(layered_rootfs_recipe(recipe, ram_base_recipe))
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
        for _target, target_config, platform_config in _current_targets():
            rootfs_packages = selected_packages(platform_config, target_config)
            packages.update(rootfs_packages)
            packages.update(
                bundle_packages(
                    platform_config,
                    target_config,
                    rootfs_packages,
                )
            )
        return frozenset(aport_build_order(tuple(packages)))
    except KeyError, OSError, TypeError, ValueError, SystemExit:
        return None


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
