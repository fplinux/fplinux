# SPDX-License-Identifier: GPL-2.0-only
"""Select causal target inputs and auxiliary shared-Linux sources."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine.registration import CLI_SOURCES
from fplinux_cli.alpine.selection import (
    aport_build_order,
    bundle_packages,
    selected_aport_graph,
    selected_packages,
    shared_aport_sources,
)
from fplinux_cli.build.inputs import selected_build_sources
from fplinux_cli.common import ROOT, canonical_json_bytes, fail, load_toml
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.manifests.kernel import kernel_config_paths
from fplinux_cli.manifests.linux import discover_linux_targets, is_shared_linux_operation
from fplinux_cli.manifests.paths import (
    profile_manifest_path,
    target_asset_lock_path,
    target_release_manifest_path,
)
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.manifests.values import sha256_value

from .capture import (
    WorkspaceFile,
    WorkspaceSnapshot,
    read_source_file,
    snapshot_from_inventory,
    snapshot_recipe,
)

if TYPE_CHECKING:
    from pathlib import Path

STAGED_BUILD_SOURCES = (
    "Containerfile",
    "THIRD_PARTY_NOTICES.md",
    "container.lock.toml",
    "environment.lock.toml",
    "alpine.lock.toml",
    "sources.lock.toml",
    "alpine/abuild.conf",
    "scripts/fplinux_cli/__init__.py",
    "scripts/fplinux_cli/alpine/__init__.py",
    "scripts/fplinux_cli/alpine/selection.py",
    "scripts/fplinux_cli/alpine/lock.py",
    "scripts/fplinux_cli/alpine/recipes.py",
    "scripts/fplinux_cli/alpine/signing.py",
    "scripts/fplinux_cli/alpine/aports.py",
    "scripts/fplinux_cli/alpine/packages.py",
    "scripts/fplinux_cli/alpine/rootfs.py",
    "scripts/fplinux_cli/alpine/rootfs_state.py",
    "scripts/fplinux_cli/alpine/rootfs_verify.py",
    "scripts/fplinux_cli/alpine/rootfs_files.py",
    "scripts/fplinux_cli/artifacts/__init__.py",
    "scripts/fplinux_cli/artifacts/records.py",
    "scripts/fplinux_cli/artifacts/bundles.py",
    "scripts/fplinux_cli/common.py",
    "scripts/fplinux_cli/manifests",
    "scripts/fplinux_cli/environment/__init__.py",
    "scripts/fplinux_cli/environment/images.py",
    "scripts/fplinux_cli/quality/__init__.py",
    "scripts/fplinux_cli/quality/source_gate.py",
    "scripts/fplinux_cli/device_data/__init__.py",
    "scripts/fplinux_cli/device_data/inputs.py",
    "scripts/fplinux_cli/reporting/__init__.py",
    "scripts/fplinux_cli/reporting/run.py",
    "scripts/fplinux_cli/reporting/process.py",
    "scripts/fplinux_cli/runtime/__init__.py",
    "scripts/fplinux_cli/runtime/ssh_transport.py",
    "scripts/fplinux_cli/workspace/__init__.py",
    "scripts/fplinux_cli/workspace/capture.py",
    "scripts/fplinux_cli/workspace/build_inputs.py",
)
_MATERIALIZED_BUILD_SOURCES = ("scripts/fplinux_cli/alpine/registration.py",)


@dataclass(frozen=True)
class SharedLinuxSources:
    """The staged Linux closure, with its causal files and ordered source operations."""

    files: tuple[tuple[str, Path], ...] = ()
    causal_paths: frozenset[str] = frozenset()
    operations: tuple[tuple[str, str, str], ...] = ()


def is_python_cache(path: Path) -> bool:
    """Return whether a repository path is generated Python bytecode."""
    relative = path.relative_to(ROOT)
    return "__pycache__" in relative.parts or path.suffix in {".pyc", ".pyo"}


def add_source_path(files: dict[str, Path], path: Path) -> None:
    """Add a regular file or a complete source directory to the closure."""
    if path.is_symlink():
        fail(f"workspace input must not be a symlink: {path}")
    if path.is_file():
        relative = path.relative_to(ROOT).as_posix()
        if not is_python_cache(path):
            files[relative] = path
        return
    if not path.is_dir():
        fail(f"workspace input is missing or not a directory: {path}")
    for child in sorted(path.rglob("*")):
        if is_python_cache(child):
            continue
        if child.is_symlink():
            fail(f"workspace input must not be a symlink: {child}")
        if child.is_dir():
            continue
        if not child.is_file():
            fail(f"workspace input must be a regular file: {child}")
        files[child.relative_to(ROOT).as_posix()] = child


def target_build_source_files(
    target: str,
    profile: str | None = None,
    *,
    build_type: str = "release",
    target_config: dict[str, Any] | None = None,
) -> list[tuple[str, Path]]:
    """Resolve only the selected target/platform build closure."""
    if target_config is None:
        target_config = load_target(target, profile, build_type=build_type)
    platform = load_platform(target_config["platform"])
    target_root = ROOT / "targets" / target
    files: dict[str, Path] = {}

    implementation_sources = (
        *STAGED_BUILD_SOURCES,
        *selected_build_sources(target_config),
        *_MATERIALIZED_BUILD_SOURCES,
    )
    for relative in implementation_sources:
        add_source_path(files, ROOT / relative)
    if target_config["linux"]["root"]["kind"] == "initramfs":
        add_source_path(files, ROOT / "alpine/ramroot-init.sh")
    rootfs_packages = selected_packages(platform, target_config, root=ROOT)
    selected_bundle_packages = bundle_packages(
        platform,
        target_config,
        rootfs_packages,
        root=ROOT,
    )
    build_packages = aport_build_order((*rootfs_packages, *selected_bundle_packages))
    for package in build_packages:
        add_source_path(files, ROOT / "alpine/aports" / package)
    shared_sources = {
        source for package in build_packages for source in shared_aport_sources(package, root=ROOT)
    }
    for source in sorted(shared_sources):
        add_source_path(files, source)
    add_source_path(files, target_root / "target.toml")
    add_source_path(files, target_release_manifest_path(target))
    add_source_path(files, target_asset_lock_path(target))
    for path in kernel_config_paths(target, target_config, platform):
        add_source_path(files, path)
    selected_profile = target_config.get("profile")
    add_source_path(files, profile_manifest_path(target, "default"))
    if selected_profile is not None:
        add_source_path(files, profile_manifest_path(target, selected_profile))
    if target_config["uboot"]["kind"] == "full":
        add_source_path(files, ROOT / target_config["uboot"]["source"])
        add_source_path(files, target_root / target_config["uboot"]["defconfig"])
        for relative in target_config["uboot"]["patches"]:
            add_source_path(files, ROOT / relative)
        for step in target_config["uboot"]["copies"]:
            add_source_path(files, ROOT / step["source"])
    add_source_path(files, target_root / target_config["bootstrap"]["source"])
    lcd_config = target_config["bootstrap"].get("lcd_config")
    if lcd_config is not None:
        add_source_path(files, target_root / lcd_config)
    for relative in target_config["linux"]["patches"]:
        add_source_path(files, target_root / relative)
    for key in ("copies", "appends"):
        for step in target_config["linux"][key]:
            add_source_path(files, target_root / step["source"])

    platform_root = ROOT / "platforms" / target_config["platform"]
    add_source_path(files, platform_root / "platform.toml")
    for relative in platform["linux"]["patches"]:
        add_source_path(files, ROOT / relative)
    for key in ("copies", "appends"):
        for step in platform["linux"][key]:
            add_source_path(files, ROOT / step["source"])
    for step in platform["bootstrap"]["shared_copies"]:
        add_source_path(files, ROOT / step["source"])
    for relative in platform["bootstrap"]["patches"]:
        add_source_path(files, ROOT / relative)
    for recipe in platform["host"]["tools"]:
        if recipe["type"] == "cc-libusb":
            add_source_path(files, ROOT / recipe["source"])
            for relative in CLI_SOURCES:
                add_source_path(files, ROOT / relative)
        elif recipe["type"] == "make-archive":
            for step in recipe["copies"]:
                add_source_path(files, ROOT / step["source"])
            for relative in recipe["patches"]:
                add_source_path(files, ROOT / relative)
    add_source_path(files, ROOT / "common/run.py")
    add_source_path(files, ROOT / "common/loader_events.py")
    add_source_path(files, platform_root / "host/adapter.py")
    return sorted(files.items())


def shared_linux_source_files(target_config: dict[str, Any]) -> SharedLinuxSources:
    """Separate board-owned copies from source operations shared by compatible builds."""
    sources = load_toml(ROOT / "sources.lock.toml")
    platform = load_platform(target_config["platform"])
    source_lock = platform["linux"]["source_lock"]
    source = sources.get(source_lock)
    if not isinstance(source, dict):
        fail(f"unknown Linux source lock: {source_lock}")
    source_sha256 = sha256_value(source.get("sha256"), f"source {source_lock} sha256")
    files: dict[str, Path] = {}
    causal_paths: set[str] = set()
    operations: list[tuple[str, str, str]] = []

    def add_operation(operation: str, source: Path, destination: str) -> None:
        relative = source.relative_to(ROOT).as_posix()
        add_source_path(files, source)
        operations.append((operation, relative, destination))
        if is_shared_linux_operation(operation):
            causal_paths.add(relative)

    for target in discover_linux_targets(ROOT, sources, source_sha256):
        target_root = ROOT / "targets" / target.name
        add_source_path(files, target_root / "target.toml")
        add_source_path(files, ROOT / "platforms" / target.config["platform"] / "platform.toml")
        for owner, source_root, config in (
            ("platform", ROOT, target.platform),
            ("target", target_root, target.config),
        ):
            for relative in config["linux"]["patches"]:
                add_operation(f"{owner}-patch", source_root / relative, "")
            for key in ("copies", "appends"):
                operation = f"{owner}-{'copy' if key == 'copies' else 'append'}"
                for step in config["linux"][key]:
                    add_operation(operation, source_root / step["source"], step["destination"])
        for relative in target.config.get("microsd", {}).get("linux_patches", []):
            add_operation("target-patch", target_root / relative, "")
    return SharedLinuxSources(
        tuple(sorted(files.items())), frozenset(causal_paths), tuple(dict.fromkeys(operations))
    )


def target_workspace_snapshot(
    target: str, profile: str | None = None, *, build_type: str = "release"
) -> WorkspaceSnapshot:
    """Read the selected build closure before deciding whether staging is needed."""
    target_config = load_target(target, profile, build_type=build_type)
    source_snapshot = snapshot_from_inventory(
        lambda: target_build_source_files(
            target,
            profile,
            build_type=build_type,
            target_config=target_config,
        )
    )
    linux_sources = shared_linux_source_files(target_config)
    causal_files = {source.path: source for source in source_snapshot.files}
    auxiliary_files = [causal_files.pop(relative) for relative in _MATERIALIZED_BUILD_SOURCES]
    for relative, source in linux_sources.files:
        if relative in causal_files:
            continue
        captured = read_source_file(relative, source)
        if relative in linux_sources.causal_paths:
            causal_files[relative] = captured
        else:
            auxiliary_files.append(captured)
    device_data_groups = target_config["device_data"]["groups"]
    captured_groups = firmware_inputs.capture_external_device_data(
        target,
        device_data_groups,
        ROOT / ".cache",
    )
    firmware_files = tuple(
        WorkspaceFile(
            firmware_inputs.snapshot_device_data_path(
                target,
                group_name,
                firmware.destination,
            ),
            firmware.contents,
            0o600,
        )
        for group_name, group in captured_groups.items()
        for firmware in group
    )
    snapshot_files = tuple(
        sorted((*causal_files.values(), *firmware_files), key=lambda item: item.path)
    )
    if len({source.path for source in snapshot_files}) != len(snapshot_files):
        fail("firmware input collides with a build workspace source path")
    platform = load_platform(target_config["platform"])
    rootfs_packages = selected_packages(platform, target_config, root=ROOT)
    selected_bundle_packages = bundle_packages(platform, target_config, rootfs_packages, root=ROOT)
    alpine_graph = canonical_json_bytes(
        {
            "rootfs_packages": rootfs_packages,
            "bundle_packages": selected_bundle_packages,
            "aports": selected_aport_graph((*rootfs_packages, *selected_bundle_packages)),
        }
    )
    return WorkspaceSnapshot(
        snapshot_files,
        snapshot_recipe(snapshot_files, build_type, linux_sources.operations, alpine_graph),
        build_type,
        tuple(auxiliary_files),
        linux_sources.operations,
        alpine_graph,
    )
