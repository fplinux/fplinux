# SPDX-License-Identifier: GPL-2.0-only
"""Select package ownership and the transitive local aport graph."""

from __future__ import annotations

from collections.abc import Mapping
from graphlib import TopologicalSorter
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail

from . import registration
from .lock import _package_id

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def _declared_packages(config: Mapping[str, object], owner: str, layer: str) -> tuple[str, ...]:
    table = config.get(layer)
    if not isinstance(table, Mapping) or set(table) != {"packages"}:
        fail(f"{owner} {layer} must contain exactly packages")
    raw = table.get("packages")
    if not isinstance(raw, list):
        fail(f"{owner} {layer} packages must be an array")
    result = tuple(
        _package_id(package, f"{owner} {layer} packages[{index}]")
        for index, package in enumerate(raw)
    )
    if len(set(result)) != len(result):
        fail(f"{owner} {layer} packages must not contain duplicates")
    return result


def _target_rootfs(
    config: Mapping[str, object],
) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    """Read normalized target base packages and its optional profile delta."""
    table = config.get("rootfs")
    package_fields = {
        "base_packages",
        "packages",
        "exclude_packages",
    }
    if not isinstance(table, Mapping) or set(table) != package_fields:
        fail(
            "normalized target rootfs must contain exactly base_packages, packages and "
            "exclude_packages"
        )

    def read(field: str) -> tuple[str, ...]:
        raw = table.get(field)
        if not isinstance(raw, list):
            fail(f"normalized target rootfs {field} must be an array")
        result = tuple(
            _package_id(package, f"normalized target rootfs {field}[{index}]")
            for index, package in enumerate(raw)
        )
        if len(set(result)) != len(result):
            fail(f"normalized target rootfs {field} must not contain duplicates")
        return result

    base_packages = read("base_packages")
    packages = read("packages")
    exclude_packages = read("exclude_packages")
    overlap = set(packages) & set(exclude_packages)
    if overlap:
        fail(
            "normalized target rootfs packages/exclude_packages conflict: "
            + ", ".join(sorted(overlap))
        )
    return base_packages, packages, exclude_packages


def aport_producer(package: str) -> str:
    """Resolve the current child APK names to the aport that builds them."""
    name = _package_id(package, "FPLinux package")
    return registration.SUBPACKAGE_APORTS.get(name, name)


def _canonical_packages(packages: Sequence[str], root: Path) -> tuple[str, ...]:
    result = tuple(sorted(_package_id(package, "FPLinux package") for package in packages))
    if len(set(result)) != len(result):
        fail("FPLinux package set must not contain duplicates")
    for package in result:
        aport = root / "alpine/aports" / aport_producer(package)
        if aport.is_symlink() or not aport.is_dir():
            fail(f"selected aport is missing or invalid: {package}")
        apkbuild = aport / "APKBUILD"
        if apkbuild.is_symlink() or not apkbuild.is_file():
            fail(f"selected aport has no regular APKBUILD: {package}")
    return result


def selected_packages(
    platform_config: Mapping[str, object],
    target_config: Mapping[str, object],
    root: Path = ROOT,
) -> tuple[str, ...]:
    """Resolve common, platform and target packages plus one selected profile delta."""
    owners: dict[str, str] = {}
    for owner, packages in (
        ("common", registration.COMMON_PACKAGES),
        ("platform", _declared_packages(platform_config, "platform", "rootfs")),
    ):
        for package in packages:
            previous = owners.get(package)
            if previous is not None:
                fail(f"package {package} is owned by both {previous} and {owner}")
            owners[package] = owner
    base_packages, packages, exclude_packages = _target_rootfs(target_config)
    duplicate_additions = set(base_packages) & set(owners)
    if duplicate_additions:
        fail(
            "target rootfs base_packages duplicate common/platform ownership: "
            + ", ".join(sorted(duplicate_additions))
        )
    for package in base_packages:
        owners[package] = "target"
    duplicate_additions = set(packages) & set(owners)
    if duplicate_additions:
        fail(
            "target profile rootfs packages duplicate base ownership: "
            + ", ".join(sorted(duplicate_additions))
        )
    for package in packages:
        owners[package] = "profile"
    unknown_excludes = set(exclude_packages) - set(owners)
    if unknown_excludes:
        fail(
            "target profile rootfs excludes a package not owned by the base rootfs: "
            + ", ".join(sorted(unknown_excludes))
        )
    for package in exclude_packages:
        del owners[package]
    text_consumers = {"fplinux-terminal", "fplinux-brightness-ui", "fplinux-showcase"}
    font_sizes = {"fplinux-font-terminus-6x12", "fplinux-font-terminus-8x16"}
    selected_fonts = set(owners) & font_sizes
    if len(selected_fonts) > 1 or (set(owners) & text_consumers and not selected_fonts):
        fail("rootfs text applications require exactly one Terminus font size")
    return _canonical_packages(tuple(owners), root)


def bundle_packages(
    platform_config: Mapping[str, object],
    target_config: Mapping[str, object],
    rootfs_packages: Sequence[str],
    root: Path = ROOT,
) -> tuple[str, ...]:
    """Resolve platform and target APKs published alongside, not in, the rootfs."""
    owners: dict[str, str] = {}
    for owner, packages in (
        ("platform", _declared_packages(platform_config, "platform", "bundle")),
        ("target", _declared_packages(target_config, "target", "bundle")),
    ):
        for package in packages:
            previous = owners.get(package)
            if previous is not None:
                fail(f"bundle package {package} is owned by both {previous} and {owner}")
            owners[package] = owner
    profile_packages = (
        set(_target_rootfs(target_config)[1]) if "rootfs" in target_config else set()
    )
    for package in profile_packages & set(rootfs_packages):
        if owners.get(package) == "platform":
            del owners[package]
    result = _canonical_packages(tuple(owners), root)
    overlap = set(result) & set(rootfs_packages)
    if overlap:
        fail(
            "packages cannot be both rootfs-selected and bundle-published: "
            + ", ".join(sorted(overlap))
        )
    return result


def shared_aport_sources(package: str, root: Path = ROOT) -> tuple[Path, ...]:
    """Return canonical project sources copied into one consuming aport."""
    name = _package_id(package, "FPLinux package")
    return tuple(root / relative for relative in registration.SHARED_APORT_SOURCES.get(name, ()))


def _aport_dependency_graph(packages: Sequence[str]) -> dict[str, tuple[str, ...]]:
    graph: dict[str, tuple[str, ...]] = {}
    pending = sorted({aport_producer(package) for package in packages})
    while pending:
        producer = pending.pop()
        if producer in graph:
            continue
        dependencies = tuple(
            sorted(
                {
                    aport_producer(name)
                    for name in registration.LOCAL_BUILD_DEPENDENCIES.get(producer, ())
                }
            )
        )
        graph[producer] = dependencies
        pending.extend(dependencies)
    return {producer: graph[producer] for producer in sorted(graph)}


def selected_aport_graph(packages: Sequence[str]) -> dict[str, dict[str, list[str]]]:
    """Describe the normalized declarations used by the selected producers."""
    graph = _aport_dependency_graph(packages)
    return {
        producer: {
            "dependencies": list(dependencies),
            "shared_sources": sorted(set(registration.SHARED_APORT_SOURCES.get(producer, ()))),
        }
        for producer, dependencies in graph.items()
    }


def local_build_dependencies(packages: Sequence[str]) -> tuple[str, ...]:
    """Select every local library needed in the consumers' build sysroot."""
    graph = _aport_dependency_graph(packages)
    return tuple(sorted({library for dependencies in graph.values() for library in dependencies}))


def aport_build_order(packages: Sequence[str]) -> tuple[str, ...]:
    """Build each producer once, after all of its declared local dependencies."""
    return tuple(TopologicalSorter(_aport_dependency_graph(packages)).static_order())
