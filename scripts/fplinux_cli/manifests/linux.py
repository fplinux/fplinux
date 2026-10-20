# SPDX-License-Identifier: GPL-2.0-only
"""Discover the Linux integrations sharing one exact upstream source archive."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, Any, Literal

from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.audio import AudioPolicyError, audio_policy_from_profile
from fplinux_cli.manifests.display import load_display
from fplinux_cli.manifests.identity import (
    IdentityError,
    validate_platform_identity,
    validate_platform_name,
    validate_target_identity,
)
from fplinux_cli.manifests.values import (
    KCONFIG_SYMBOL,
    TARGET_NAME,
    exact_table,
    nonempty_string,
    path_array,
    path_steps,
    relative_value,
    sha256_value,
    string_array,
)

if TYPE_CHECKING:
    from collections.abc import Iterator, Mapping
    from pathlib import Path

_REPOSITORY_SOURCE_BASE = PurePosixPath()

_FORBIDDEN_STORAGE_DTB_MARKERS = ("spi-nand", "mtd", "ubi", "emmc")
_FORBIDDEN_STORAGE_CONFIG = (
    "CONFIG_MTD=y",
    "CONFIG_UBI=y",
    "CONFIG_SCSI=y",
    "CONFIG_ATA=y",
    "CONFIG_NVME_CORE=y",
)


@dataclass(frozen=True)
class LinuxTarget:
    """Only the board and platform inputs needed to prepare shared Linux sources."""

    name: str
    config: dict[str, Any]
    platform: dict[str, Any]


def linux_dt_config_checks(value: object, name: str) -> list[dict[str, str]]:
    """Validate compiled device-tree ownership checks declared by one Linux owner."""
    if not isinstance(value, list):
        fail(f"{name} must be an array")
    result: list[dict[str, str]] = []
    identities: set[tuple[str, str, str | None]] = set()
    for index, raw in enumerate(value):
        item_name = f"{name}[{index}]"
        fields = {"path", "compatible", "config"}
        if isinstance(raw, dict) and "property" in raw:
            fields.add("property")
        check = exact_table(raw, fields, item_name)
        path = nonempty_string(check.get("path"), f"{item_name} path")
        if not path.startswith("/") or (
            path != "/" and any(part in {"", ".", ".."} for part in path[1:].split("/"))
        ):
            fail(f"{item_name} path must be a canonical absolute device-tree path")
        compatible = nonempty_string(check.get("compatible"), f"{item_name} compatible")
        symbol = nonempty_string(check.get("config"), f"{item_name} config")
        if KCONFIG_SYMBOL.fullmatch(symbol) is None:
            fail(f"{item_name} config must be a CONFIG_* symbol")
        property_name = (
            nonempty_string(check["property"], f"{item_name} property")
            if "property" in check
            else None
        )
        identity = (path, compatible, property_name)
        if identity in identities:
            condition = f" property {property_name}" if property_name is not None else ""
            fail(f"{name} repeats ownership for {path} compatible {compatible}{condition}")
        identities.add(identity)
        requirement = {"path": path, "compatible": compatible, "config": symbol}
        if property_name is not None:
            requirement["property"] = property_name
        result.append(requirement)
    return result


def linux_storage_policy(
    additional_config: object, additional_dtb_markers: object
) -> dict[str, list[str]]:
    """Combine the mandatory internal-storage exclusions with board restrictions."""
    return {
        "forbidden_config": [
            *string_array(
                additional_config, "target linux additional_forbidden_config", allow_empty=True
            ),
            *_FORBIDDEN_STORAGE_CONFIG,
        ],
        "forbidden_dtb_markers": [
            *_FORBIDDEN_STORAGE_DTB_MARKERS,
            *string_array(
                additional_dtb_markers,
                "target linux additional_forbidden_dtb_markers",
                allow_empty=True,
            ),
        ],
    }


def linux_declarations(
    linux: dict[str, Any],
    *,
    owner: Literal["platform", "target"],
    kind: Literal["patch", "copy", "append"],
    source_base: PurePosixPath = _REPOSITORY_SOURCE_BASE,
) -> Iterator[tuple[str, str, str]]:
    """Enumerate one normalized operation group without choosing consumer order."""
    operation = f"{owner}-{kind}"
    if kind == "patch":
        for relative in linux["patches"]:
            yield operation, (source_base / relative).as_posix(), ""
        return
    key = "copies" if kind == "copy" else "appends"
    for step in linux[key]:
        yield operation, (source_base / step["source"]).as_posix(), step["destination"]


def is_shared_linux_operation(operation: str) -> bool:
    """Identify source operations that can affect every compatible board build."""
    return operation in {
        "platform-patch",
        "platform-copy",
        "target-patch",
        "platform-append",
        "target-append",
    }


def linux_copy_steps(  # noqa: PLR0913 -- source declarations and capture inputs stay explicit.
    value: object,
    overlay: object,
    name: str,
    *,
    root: Path,
    source_base: PurePosixPath = _REPOSITORY_SOURCE_BASE,
    captured: Mapping[str, bytes] | None = None,
) -> list[dict[str, str]]:
    """Expand one owner overlay and explicit copies into independent file records."""
    steps = path_steps(value, f"{name} copies")
    overlay_relative = relative_value(overlay, f"{name} overlay")
    overlay_path = source_base / overlay_relative
    if captured is None:
        directory = root / overlay_path
        if directory.is_symlink() or not directory.is_dir():
            fail(f"{name} overlay must be a regular source directory: {directory}")
        paths = []
        for source in sorted(directory.rglob("*")):
            if source.is_symlink() or not (source.is_file() or source.is_dir()):
                fail(f"{name} overlay entry must be a regular file or directory: {source}")
            if source.is_file():
                paths.append(PurePosixPath(source.relative_to(root).as_posix()))
    else:
        paths = [
            path
            for relative in sorted(captured)
            if (path := PurePosixPath(relative)).is_relative_to(overlay_path)
            and path != overlay_path
        ]
    for path in paths:
        steps.append(
            {
                "source": path.relative_to(source_base).as_posix(),
                "destination": path.relative_to(overlay_path).as_posix(),
            }
        )
    owners: dict[str, str] = {}
    for step in steps:
        destination = step["destination"]
        if destination in owners:
            fail(
                f"Linux copy destination has multiple owners: {destination} "
                f"({owners[destination]}, {step['source']})"
            )
        owners[destination] = step["source"]
    return sorted(steps, key=lambda step: (step["destination"], step["source"]))


def normalize_linux_integration(
    value: object,
    name: str,
    *,
    root: Path,
    source_base: PurePosixPath = _REPOSITORY_SOURCE_BASE,
    captured: Mapping[str, bytes] | None = None,
) -> dict[str, Any]:
    """Normalize Linux operations for full manifests, discovery and captured inputs."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    return {
        "patches": path_array(value.get("patches"), f"{name} patches", allow_empty=True),
        "copies": linux_copy_steps(
            value.get("copies"),
            value.get("overlay"),
            name,
            root=root,
            source_base=source_base,
            captured=captured,
        ),
        "appends": path_steps(value.get("appends"), f"{name} appends"),
        "dt_config_checks": linux_dt_config_checks(
            value.get("dt_config_checks", []), f"{name} dt_config_checks"
        ),
    }


def _linux_manifest(
    root: Path, relative: str, captured: Mapping[str, bytes] | None
) -> dict[str, Any]:
    """Read a Linux peer declaration from either current or already captured bytes."""
    if captured is None:
        path = root / relative
        if path.parent.is_symlink() or path.is_symlink() or not path.is_file():
            fail(f"Linux manifest is missing or invalid: {path}")
        return load_toml(path)
    contents = captured.get(relative)
    if contents is None:
        fail(f"Linux manifest is missing from captured sources: {relative}")
    try:
        return tomllib.loads(contents.decode("utf-8"))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        fail(f"invalid TOML in {relative}: {error}")


def discover_linux_targets(
    root: Path,
    sources: dict[str, Any],
    source_sha256: str,
    *,
    captured: Mapping[str, bytes] | None = None,
) -> tuple[LinuxTarget, ...]:
    """Find Linux-only peer inputs by archive digest, across platform and ARCH boundaries."""
    sha256_value(source_sha256, "Linux source")
    platforms: dict[str, dict[str, Any]] = {}
    targets: list[LinuxTarget] = []
    manifests = (
        sorted((root / "targets").glob("*/target.toml"))
        if captured is None
        else [
            root / path
            for relative in sorted(captured)
            if len((path := PurePosixPath(relative)).parts) == 3
            and path.parts[0] == "targets"
            and path.name == "target.toml"
        ]
    )
    for manifest in manifests:
        target = manifest.parent.name
        if TARGET_NAME.fullmatch(target) is None or (
            captured is None
            and (manifest.parent.is_symlink() or manifest.is_symlink() or not manifest.is_file())
        ):
            continue
        raw = _linux_manifest(root, manifest.relative_to(root).as_posix(), captured)
        try:
            platform_name = validate_platform_name(
                raw.get("platform"), f"target {target} platform"
            )
        except IdentityError as error:
            fail(str(error))
        if platform_name not in platforms:
            platforms[platform_name] = _linux_manifest(
                root, f"platforms/{platform_name}/platform.toml", captured
            )
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
        linux = normalize_linux_integration(
            platform_linux, f"platform {platform_name} linux", root=root, captured=captured
        )
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
            "linux": normalize_linux_integration(
                raw.get("linux"),
                f"target {target} linux",
                root=root,
                source_base=PurePosixPath("targets") / target,
                captured=captured,
            ),
            "display": load_display(target, raw.get("display"), root=root, captured=captured),
        }
        try:
            config["audio_policy"] = audio_policy_from_profile(raw.get("audio_profile"))
        except AudioPolicyError as error:
            fail(str(error))
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
