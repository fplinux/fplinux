# SPDX-License-Identifier: GPL-2.0-only
"""Discover the Linux integrations sharing one exact upstream source archive."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.identity import (
    IdentityError,
    validate_platform_identity,
    validate_platform_name,
    validate_target_identity,
)
from fplinux_cli.manifests.values import (
    TARGET_NAME,
    nonempty_string,
    path_array,
    path_steps,
    relative_value,
    sha256_value,
)

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class LinuxTarget:
    """Only the board and platform inputs needed to prepare shared Linux sources."""

    name: str
    config: dict[str, Any]
    platform: dict[str, Any]


def is_shared_linux_operation(operation: str) -> bool:
    """Identify source operations that can affect every compatible board build."""
    return operation in {
        "platform-patch",
        "platform-copy",
        "target-patch",
        "platform-append",
        "target-append",
    }


def _projection(value: object, name: str) -> dict[str, Any]:
    """Normalize source operations without loading unrelated build configuration."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    return {
        "patches": path_array(value.get("patches"), f"{name} patches", allow_empty=True),
        "copies": path_steps(value.get("copies"), f"{name} copies"),
        "appends": path_steps(value.get("appends"), f"{name} appends"),
    }


def discover_linux_targets(
    root: Path,
    sources: dict[str, Any],
    source_sha256: str,
) -> tuple[LinuxTarget, ...]:
    """Find Linux-only peer inputs by archive digest, across platform and ARCH boundaries."""
    sha256_value(source_sha256, "Linux source")
    platforms: dict[str, dict[str, Any]] = {}
    targets: list[LinuxTarget] = []
    for manifest in sorted((root / "targets").glob("*/target.toml")):
        target = manifest.parent.name
        if (
            TARGET_NAME.fullmatch(target) is None
            or manifest.parent.is_symlink()
            or manifest.is_symlink()
            or not manifest.is_file()
        ):
            continue
        raw = load_toml(manifest)
        try:
            platform_name = validate_platform_name(
                raw.get("platform"), f"target {target} platform"
            )
        except IdentityError as error:
            fail(str(error))
        if platform_name not in platforms:
            platform_path = root / "platforms" / platform_name / "platform.toml"
            if (
                platform_path.parent.is_symlink()
                or platform_path.is_symlink()
                or not platform_path.is_file()
            ):
                fail(f"unknown platform: {platform_name}")
            platforms[platform_name] = load_toml(platform_path)
        raw_platform = platforms[platform_name]
        platform_linux = raw_platform.get("linux")
        if not isinstance(platform_linux, dict):
            fail(f"platform {platform_name} linux must be a table")
        source_lock = nonempty_string(
            platform_linux.get("source_lock"), f"platform {platform_name} linux source_lock"
        )
        source = sources.get(source_lock)
        if not isinstance(source, dict):
            fail(f"unknown Linux source lock: {source_lock}")
        if sha256_value(source.get("sha256"), f"source {source_lock} sha256") != source_sha256:
            continue

        try:
            identity = validate_target_identity(raw.get("identity"), f"target {target} identity")
            platform_identity = validate_platform_identity(
                raw_platform.get("identity"), f"platform {platform_name} identity"
            )
        except IdentityError as error:
            fail(str(error))
        linux = _projection(platform_linux, f"platform {platform_name} linux")
        linux["source_lock"] = source_lock
        linux["arch"] = nonempty_string(
            platform_linux.get("arch"), f"platform {platform_name} linux arch"
        )
        for key in ("dts_directory", "platform_identity_header"):
            linux[key] = relative_value(
                platform_linux.get(key), f"platform {platform_name} linux {key}"
            )
        config: dict[str, Any] = {
            "identity": identity,
            "platform": platform_name,
            "linux": _projection(raw.get("linux"), f"target {target} linux"),
        }
        if "microsd" in raw:
            microsd = raw["microsd"]
            if not isinstance(microsd, dict):
                fail(f"target {target} microsd must be a table")
            config["microsd"] = {
                "linux_patches": path_array(
                    microsd.get("linux_patches"),
                    f"target {target} microsd linux_patches",
                    allow_empty=True,
                )
            }
        targets.append(
            LinuxTarget(target, config, {"identity": platform_identity, "linux": linux})
        )
    return tuple(targets)
