# SPDX-License-Identifier: GPL-2.0-only
"""Select causal target inputs and auxiliary shared-Linux sources."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, Any, Literal

from fplinux_cli.alpine.registration import CLI_SOURCES
from fplinux_cli.alpine.selection import (
    aport_build_order,
    bundle_packages,
    selected_aport_graph,
    selected_packages,
    shared_aport_sources,
)
from fplinux_cli.build.inputs import selected_build_sources
from fplinux_cli.build.runtime import runtime_source_files
from fplinux_cli.build.sources import source_lock_entry
from fplinux_cli.common import ROOT, canonical_json_bytes, fail
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.environment.definition import ENVIRONMENT_RECIPE_INPUTS
from fplinux_cli.manifests.assets import resolve_asset_sources
from fplinux_cli.manifests.kernel import kernel_config_paths
from fplinux_cli.manifests.linux import (
    discover_linux_targets,
    is_shared_linux_operation,
    linux_declarations,
)
from fplinux_cli.manifests.loaders import loader_sources
from fplinux_cli.manifests.paths import (
    platform_asset_lock_path,
    profile_manifest_path,
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
    from collections.abc import Iterator, Mapping
    from pathlib import Path

    from fplinux_cli.manifests.linux import LinuxTarget

STAGED_BUILD_SOURCES = (
    *ENVIRONMENT_RECIPE_INPUTS,
    "THIRD_PARTY_NOTICES.md",
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

    files: tuple[WorkspaceFile, ...] = ()
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
    if platform["bootstrap"]["backend"] == "ums9117":
        add_source_path(files, platform_asset_lock_path(target_config["platform"]))
    display_source = target_config["display"]["source"]
    if display_source is not None:
        add_source_path(files, target_root / display_source)
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
    if platform["bootstrap"]["backend"] == "sc9820e":
        add_source_path(files, target_root / target_config["loader"]["build"]["source"])
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
    if platform["bootstrap"]["backend"] == "sc9820e":
        add_source_path(files, platform_root / "bootstrap")
    else:
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
    for path in runtime_source_files(target, target_config, root=ROOT).values():
        add_source_path(files, path)
    return sorted(files.items())


def _linux_source_operations(
    targets: tuple[LinuxTarget, ...],
) -> Iterator[tuple[str, str, str]]:
    """Name the peer operations whose topology participates in a workspace recipe."""
    for target in targets:
        owners: tuple[tuple[Literal["platform", "target"], PurePosixPath, dict[str, Any]], ...] = (
            ("platform", PurePosixPath(), target.platform["linux"]),
            ("target", PurePosixPath("targets") / target.name, target.config["linux"]),
        )
        kinds: tuple[Literal["patch", "copy", "append"], ...] = ("patch", "copy", "append")
        for owner, source_base, linux in owners:
            for kind in kinds:
                yield from linux_declarations(
                    linux, owner=owner, kind=kind, source_base=source_base
                )
        for relative in target.config.get("microsd", {}).get("linux_patches", []):
            yield "target-patch", f"targets/{target.name}/{relative}", ""


def shared_linux_source_files(
    target_config: dict[str, Any],
    sources: dict[str, Any],
    *,
    captured_files: Mapping[str, WorkspaceFile],
) -> SharedLinuxSources:
    """Capture peer sources once and classify operations from those exact bytes."""
    platform_path = f"platforms/{target_config['platform']}/platform.toml"
    platform = _captured_toml(captured_files[platform_path])
    source_lock = platform["linux"]["source_lock"]
    source = sources.get(source_lock)
    if not isinstance(source, dict):
        fail(f"unknown Linux source lock: {source_lock}")
    source_sha256 = sha256_value(source.get("sha256"), f"source {source_lock} sha256")
    targets = discover_linux_targets(ROOT, sources, source_sha256)
    files: dict[str, Path] = {}
    for target in targets:
        target_root = ROOT / "targets" / target.name
        add_source_path(files, target_root / "target.toml")
        display_source = target.config["display"]["source"]
        if display_source is not None:
            add_source_path(files, target_root / display_source)
        add_source_path(files, ROOT / "platforms" / target.config["platform"] / "platform.toml")
    for _operation, relative, _destination in _linux_source_operations(targets):
        add_source_path(files, ROOT / relative)
    captured = {
        relative: captured_files[relative]
        if relative in captured_files
        else read_source_file(relative, source_path)
        for relative, source_path in sorted(files.items())
    }
    targets = discover_linux_targets(
        ROOT,
        sources,
        source_sha256,
        captured={relative: source_file.contents for relative, source_file in captured.items()},
    )
    operations = tuple(dict.fromkeys(_linux_source_operations(targets)))
    causal_paths = frozenset(
        relative
        for operation, relative, _destination in operations
        if is_shared_linux_operation(operation)
    )
    return SharedLinuxSources(tuple(captured.values()), causal_paths, operations)


def _captured_toml(source: WorkspaceFile) -> dict[str, Any]:
    """Parse an immutable TOML input while retaining its diagnostic path."""
    try:
        return tomllib.loads(source.contents.decode("utf-8"))
    except tomllib.TOMLDecodeError as error:
        fail(f"invalid TOML in {source.path}: {error}")


def _captured_source_declarations(
    files: dict[str, WorkspaceFile], target: str, platform_name: str
) -> tuple[dict[str, Any], bytes]:
    """Select source and loader records from the captured checkout bytes."""
    source_file = files["sources.lock.toml"]
    sources = _captured_toml(source_file)
    platform_file = files[f"platforms/{platform_name}/platform.toml"]
    platform = _captured_toml(platform_file)
    target_file = files[f"targets/{target}/target.toml"]
    target_config = _captured_toml(target_file)
    source_names = {
        platform["linux"]["source_lock"],
        *(
            tool["source_lock"]
            for tool in platform["host"]["tools"]
            if tool["type"] == "make-archive"
        ),
    }
    if platform["bootstrap"]["backend"] == "ums9117":
        source_names.add(platform["bootstrap"]["vendor_source_lock"])
    else:
        source_names.update(
            source.source_lock
            for source in loader_sources(target_config["loader"]["build"], sources)
        )
    selected = {name: source_lock_entry(sources, name) for name in sorted(source_names)}
    asset_declaration: dict[str, Any] = {"records": []}
    if platform["bootstrap"]["backend"] == "ums9117":
        asset_file = files[f"platforms/{platform_name}/loader/assets.lock.toml"]
        assets = resolve_asset_sources(_captured_toml(asset_file))
        asset_declaration = {"mode": asset_file.mode, "records": assets}
    declarations = canonical_json_bytes(
        {
            "sources": {"mode": source_file.mode, "records": selected},
            "assets": asset_declaration,
        }
    )
    return sources, declarations


def target_workspace_snapshot(
    target: str,
    profile: str | None = None,
    *,
    build_type: str = "release",
    include_device_data: bool = True,
) -> WorkspaceSnapshot:
    """Capture selected sources and, by default, locally prepared device data."""
    target_config = load_target(target, profile, build_type=build_type)
    source_snapshot = snapshot_from_inventory(
        lambda: target_build_source_files(
            target,
            profile,
            build_type=build_type,
            target_config=target_config,
        )
    )
    causal_files = {source.path: source for source in source_snapshot.files}
    sources, source_declarations = _captured_source_declarations(
        causal_files, target, target_config["platform"]
    )
    linux_sources = shared_linux_source_files(target_config, sources, captured_files=causal_files)
    materialized_sources = [
        *_MATERIALIZED_BUILD_SOURCES,
        "sources.lock.toml",
    ]
    asset_lock = f"platforms/{target_config['platform']}/loader/assets.lock.toml"
    if asset_lock in causal_files:
        materialized_sources.append(asset_lock)
    auxiliary_files = [causal_files.pop(relative) for relative in materialized_sources]
    for captured in linux_sources.files:
        relative = captured.path
        if relative in causal_files:
            continue
        if relative in linux_sources.causal_paths:
            causal_files[relative] = captured
        else:
            auxiliary_files.append(captured)
    device_data_groups = target_config["device_data"]["groups"]
    build_uses_device_data = firmware_inputs.rootfs_receives_firmware(
        root_kind=target_config["linux"]["root"]["kind"],
        platform=target_config["platform"],
    )
    captured_groups = (
        firmware_inputs.capture_external_device_data(target, device_data_groups, ROOT / ".cache")
        if include_device_data and build_uses_device_data
        else {}
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
        files=snapshot_files,
        recipe=snapshot_recipe(
            snapshot_files,
            build_type,
            linux_sources.operations,
            alpine_graph,
            source_declarations,
        ),
        build_type=build_type,
        auxiliary_files=tuple(auxiliary_files),
        linux_operations=linux_sources.operations,
        alpine_graph=alpine_graph,
        source_declarations=source_declarations,
    )
