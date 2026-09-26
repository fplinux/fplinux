# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the selected target Linux source tree."""

from __future__ import annotations

import json
import shutil
import tarfile
from typing import TYPE_CHECKING, Any

from fplinux_cli import linux_state, profile_layout
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.common import fail, sha256_bytes, sha256_file
from fplinux_cli.identity_codegen import (
    LINUX_IDENTITY_DTSI,
    linux_identity_dtsi,
    linux_machine_binding,
    linux_machine_binding_path,
    linux_platform_identity_header,
)
from fplinux_cli.linux_state import LinuxStateError, PreparedLinuxState
from fplinux_cli.manifests.targets import load_target

if TYPE_CHECKING:
    from pathlib import Path


def integration_inputs(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> list[tuple[str, str, str, Path]]:
    """Return typed Linux recipe inputs in projection order."""
    platform_linux = platform["linux"]
    target_linux = target_config["linux"]
    result = [
        (
            "platform-patch",
            relative,
            "",
            inputs_build.require_file(inputs_build.root_source(relative)),
        )
        for relative in platform_linux["patches"]
    ]

    def add_steps(operation: str, steps: list[dict[str, Any]], *, platform_owned: bool) -> None:
        for step in steps:
            relative = step["source"]
            if platform_owned:
                identity = relative
                path = inputs_build.root_source(relative)
            else:
                identity = f"targets/{target}/{relative}"
                path = inputs_build.target_source(target, relative)
            result.append(
                (operation, identity, step["destination"], inputs_build.require_file(path))
            )

    add_steps("platform-copy", platform_linux["copies"], platform_owned=True)
    add_steps("target-copy", target_linux["copies"], platform_owned=False)
    result.extend(
        (
            "target-patch",
            f"targets/{target}/{relative}",
            "",
            inputs_build.require_file(inputs_build.target_source(target, relative)),
        )
        for relative in target_linux["patches"]
    )
    add_steps("platform-append", platform_linux["appends"], platform_owned=True)
    add_steps("target-append", target_linux["appends"], platform_owned=False)
    return result


PROFILE_ROOT_DTSI = "fplinux-root.dtsi"


def generated_linux_files(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, bytes]:
    """Return exact generated Linux files keyed by destination."""
    target_identity = target_config["identity"]
    platform_identity = platform["identity"]
    platform_linux = platform["linux"]
    dts_directory = platform_linux["dts_directory"]
    files = {
        f"{dts_directory}/{LINUX_IDENTITY_DTSI}": linux_identity_dtsi(
            target_identity, platform_identity
        ),
        platform_linux["platform_identity_header"]: linux_platform_identity_header(
            platform_identity
        ),
        linux_machine_binding_path(target_identity): linux_machine_binding(
            target_identity, platform_identity
        ),
    }
    root = target_config["linux"]["root"]
    files[f"{dts_directory}/{PROFILE_ROOT_DTSI}"] = profile_layout.root_bootargs_dtsi(root)
    return files


def linux_recipe_digest(
    linux_source: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> str:
    """Hash the pinned Linux release and every ordered projection operation."""
    version = linux_source.get("version")
    if not isinstance(version, str) or not version:
        fail("Linux source version must be a non-empty string")
    source_digest = inputs_build.require_sha256(linux_source.get("sha256"), "Linux source")
    manifest = {
        "version": version,
        "sha256": source_digest,
        "integration": [
            {
                "operation": operation,
                "source": relative,
                "destination": destination,
                "sha256": sha256_file(path),
            }
            for operation, relative, destination, path in integration_inputs(
                target, target_config, platform
            )
        ],
        "generated": [
            {"destination": destination, "sha256": sha256_bytes(contents)}
            for destination, contents in sorted(
                generated_linux_files(target_config, platform).items()
            )
        ],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    return sha256_bytes(encoded)


def profile_linux_source_path(parent: Path, target: str, profile: str) -> Path:
    """Create a profile-only prepared-Linux slot beside, never inside, the default tree."""
    root = inputs_build.require_directory(parent.parent)
    source = root
    for component in ("profiles", target, profile):
        source /= component
        if source.exists():
            inputs_build.require_directory(source)
        else:
            source.mkdir()
    return inputs_build.require_directory(source)


def discard_profile_linux_source(parent: Path, target: str, profile: str) -> None:
    """Discard a stale dedicated source tree after a profile now shares default sources."""
    root = inputs_build.require_directory(parent.parent)
    profiles = root / "profiles"
    if profiles.is_symlink():
        fail(f"profile Linux source root must not be a symlink: {profiles}")
    if not profiles.exists():
        return
    inputs_build.require_directory(profiles)
    target_slot = profiles / target
    if target_slot.is_symlink():
        fail(f"profile Linux target slot must not be a symlink: {target_slot}")
    if not target_slot.exists():
        return
    inputs_build.require_directory(target_slot)
    source = target_slot / profile
    if source.is_symlink():
        fail(f"profile Linux source slot must not be a symlink: {source}")
    if not source.exists():
        return
    if not source.is_dir():
        fail(f"profile Linux source slot is invalid: {source}")
    shutil.rmtree(source)


def prepared_linux_staging_path(parent: Path, target: str, profile: str | None) -> Path:
    """Create one empty, bounded staging slot for a default or named profile source tree."""
    root = inputs_build.require_directory(parent.parent)
    staging = root
    components: tuple[str, ...]
    if profile is None:
        components = ("staging", target, "default")
    else:
        components = ("staging", target, "profiles", profile)
    for component in components:
        staging /= component
        if staging.is_symlink():
            fail(f"prepared Linux staging slot must not be a symlink: {staging}")
        if staging.exists():
            if not staging.is_dir():
                fail(f"prepared Linux staging slot is invalid: {staging}")
        else:
            staging.mkdir()
    if staging.is_symlink() or not staging.is_dir():
        fail(f"prepared Linux staging slot is invalid: {staging}")
    shutil.rmtree(staging)
    staging.mkdir()
    return staging


def discard_prepared_linux_staging(staging: Path) -> None:
    """Discard only one real staging slot after publish or a failed preparation."""
    if staging.is_symlink():
        fail(f"prepared Linux staging slot must not be a symlink: {staging}")
    if not staging.exists():
        return
    if not staging.is_dir():
        fail(f"prepared Linux staging slot is invalid: {staging}")
    shutil.rmtree(staging)


def prepare_linux(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> tuple[Path, PreparedLinuxState]:
    """Create or exactly reuse the one receipt-validated Linux tree for a target."""
    platform_linux = platform["linux"]
    linux = sources_build.source_lock_entry(sources, platform_linux["source_lock"])
    recipe = linux_recipe_digest(linux, target, target_config, platform)
    version = linux["version"]
    source_digest = inputs_build.require_sha256(linux.get("sha256"), "Linux source")
    platform_patches = [
        inputs_build.require_file(inputs_build.root_source(relative))
        for relative in platform_linux["patches"]
    ]
    target_patches = [
        inputs_build.require_file(inputs_build.target_source(target, relative))
        for relative in target_config["linux"]["patches"]
    ]
    copies = [
        *sources_build.resolve_steps(target, platform_linux["copies"], platform_owned=True),
        *sources_build.resolve_steps(
            target, target_config["linux"]["copies"], platform_owned=False
        ),
    ]
    appends = [
        *sources_build.resolve_steps(target, platform_linux["appends"], platform_owned=True),
        *sources_build.resolve_steps(
            target, target_config["linux"]["appends"], platform_owned=False
        ),
    ]
    generated_files = generated_linux_files(target_config, platform)
    try:
        parent = linux_state.ensure_sources_directory(inputs_build.CACHE)
    except LinuxStateError as error:
        fail(str(error))
    source = parent / target
    profile = inputs_build.selected_profile(target_config)
    if profile is not None:
        default_config = load_target(target)
        default_recipe = linux_recipe_digest(linux, target, default_config, platform)
        if default_recipe != recipe:
            source = profile_linux_source_path(parent, target, profile)
        else:
            discard_profile_linux_source(parent, target, profile)

    def apply_projection(destination: Path) -> None:
        sources_build.apply_patches(destination, platform_patches)
        sources_build.copy_steps(destination, copies)
        sources_build.apply_patches(destination, target_patches)
        sources_build.append_steps(destination, appends)
        sources_build.write_generated_files(destination, generated_files, owner="Linux projection")

    prepared = linux_state.inspect_prepared_linux(source, recipe)
    if prepared is not None:
        return source, prepared

    archive = sources_build.fetch(
        linux.get("url"),
        source_digest,
        inputs_build.CACHE / "downloads/linux",
        f"linux-{version}.tar.xz",
    )
    staging = prepared_linux_staging_path(parent, target, profile)
    try:
        with tarfile.open(archive, "r:xz") as tar:
            tar.extractall(staging, filter="data")
        extracted = staging / f"linux-{version}"
        inputs_build.require_file(extracted / "Makefile")
        try:
            apply_projection(extracted)
            state = linux_state.seal_prepared_linux(extracted, recipe)
            linux_state.publish_prepared_linux(source, extracted)
        except LinuxStateError as error:
            fail(str(error))
    finally:
        discard_prepared_linux_staging(staging)
    return source, state
