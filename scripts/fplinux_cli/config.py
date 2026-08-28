# SPDX-License-Identifier: GPL-2.0-only
"""Load and validate targets, platforms and the build environment lock."""

from __future__ import annotations

import hashlib
import os
import re
import tomllib
from pathlib import Path
from typing import Any, Protocol

from .alpine_state import COMMON_PACKAGES
from .common import ROOT, fail, relative_name
from .identity import (
    IdentityError,
    validate_platform_identity,
    validate_target_identity,
)
from .identity_codegen import validate_record_prefix

TARGET_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")
VALUE_NAME = re.compile(r"[A-Za-z0-9._-]+")
KCONFIG_SYMBOL = re.compile(r"CONFIG_[A-Z0-9_]+")
GIT_COMMIT = re.compile(r"[0-9a-f]{40}")
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def exact_table(value: object, keys: set[str], name: str) -> dict[str, Any]:
    """Require an exact-key TOML table."""
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"{name} must contain exactly: {', '.join(sorted(keys))}")
    return value


def nonempty_string(value: object, name: str) -> str:
    """Require a non-empty string."""
    if not isinstance(value, str) or not value:
        fail(f"{name} must be a non-empty string")
    return value


def relative_value(value: object, name: str) -> str:
    """Require a normalized relative path."""
    return relative_name(value, field=name)


def basename_value(value: object, name: str) -> str:
    """Require one normalized relative filename without directories."""
    result = relative_value(value, name)
    if Path(result).name != result:
        fail(f"{name} must be a filename without directories")
    return result


def string_array(value: object, name: str, *, allow_empty: bool = False) -> list[str]:
    """Require an array of unique non-empty strings."""
    if not isinstance(value, list) or (not value and not allow_empty):
        qualifier = "an array" if allow_empty else "a non-empty array"
        fail(f"{name} must be {qualifier}")
    result = [nonempty_string(item, name) for item in value]
    if len(result) != len(set(result)):
        fail(f"{name} must not contain duplicates")
    return result


def path_array(value: object, name: str, *, allow_empty: bool = False) -> list[str]:
    """Require an array of normalized relative paths."""
    result = string_array(value, name, allow_empty=allow_empty)
    return [relative_value(item, name) for item in result]


def package_array(value: object, name: str) -> list[str]:
    """Require an array of unique, path-safe package identifiers."""
    result = string_array(value, name, allow_empty=True)
    for package in result:
        relative_value(package, name)
        if VALUE_NAME.fullmatch(package) is None:
            fail(f"{name} must contain only value-name package identifiers")
    return result


def kconfig_symbol_array(value: object, name: str) -> list[str]:
    """Require unique Kconfig symbols, without assignments or values."""
    result = string_array(value, name, allow_empty=True)
    for symbol in result:
        if KCONFIG_SYMBOL.fullmatch(symbol) is None:
            fail(f"{name} must contain only CONFIG_* symbols")
    return result


def integer_value(
    value: object,
    name: str,
    *,
    bounds: tuple[int, int],
    alignment: int = 1,
) -> int:
    """Require a bounded, optionally aligned integer."""
    minimum, maximum = bounds
    if type(value) is not int or not minimum <= value <= maximum:
        fail(f"{name} must be an integer in {minimum}..{maximum}")
    if value % alignment:
        fail(f"{name} must be aligned to {alignment} bytes")
    return value


def path_steps(value: object, name: str) -> list[dict[str, str]]:
    """Validate typed source-to-destination projection steps."""
    if not isinstance(value, list):
        fail(f"{name} must be an array")
    result: list[dict[str, str]] = []
    for index, raw in enumerate(value):
        step = exact_table(raw, {"source", "destination"}, f"{name}[{index}]")
        result.append(
            {
                "source": relative_value(step.get("source"), f"{name}[{index}] source"),
                "destination": relative_value(
                    step.get("destination"),
                    f"{name}[{index}] destination",
                ),
            }
        )
    return result


def validate_usb(value: object, name: str, *, interface_fields: bool = False) -> dict[str, Any]:
    """Validate USB identity and timeout metadata."""
    base_fields = {"vendor_id", "product_id", "wait_seconds"}
    fields = base_fields | ({"keyboard_interface"} if interface_fields else set())
    table = exact_table(value, fields, name)
    integer_value(table.get("vendor_id"), f"{name} vendor_id", bounds=(0, 0xFFFF))
    integer_value(table.get("product_id"), f"{name} product_id", bounds=(0, 0xFFFF))
    integer_value(table.get("wait_seconds"), f"{name} wait_seconds", bounds=(1, 3600))
    for field in ("keyboard_interface",):
        if field in table:
            integer_value(table[field], f"{name} {field}", bounds=(0, 255))
    return table


def _sha256(value: object, name: str) -> str:
    """Require one lowercase SHA-256 digest."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def load_asset_lock(path: Path) -> list[dict[str, Any]]:
    """Load the current pinned asset outputs used to construct a RAM bundle."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    root = exact_table(document, {"source"}, "asset lock")
    sources = root.get("source")
    if not isinstance(sources, list) or not sources:
        fail("asset lock source must be a non-empty array")

    source_ids: set[str] = set()
    roles: set[str] = set()
    paths: set[str] = set()
    result: list[dict[str, Any]] = []
    for index, raw_source in enumerate(sources):
        name = f"asset source[{index}]"
        source = exact_table(
            raw_source,
            {"id", "kind", "url", "sha256", "cache_name", "license", "output"},
            name,
        )
        source_id = nonempty_string(source.get("id"), f"{name} id")
        if source_id in source_ids:
            fail(f"{name} id must be unique")
        source_ids.add(source_id)
        kind = source.get("kind")
        if kind not in {"file", "7z"}:
            fail(f"{name} kind must be file or 7z")
        url = nonempty_string(source.get("url"), f"{name} url")
        if not url.startswith("https://"):
            fail(f"{name} url must use HTTPS")
        _sha256(source.get("sha256"), f"{name} source")
        relative_value(source.get("cache_name"), f"{name} cache_name")
        nonempty_string(source.get("license"), f"{name} license")
        outputs = source.get("output")
        if not isinstance(outputs, list) or not outputs:
            fail(f"{name} output must be a non-empty array")
        normalized_outputs: list[dict[str, Any]] = []
        for output_index, raw_output in enumerate(outputs):
            output_name = f"{name} output[{output_index}]"
            keys = (
                {"role", "path", "sha256", "member"}
                if kind == "7z"
                else {
                    "role",
                    "path",
                    "sha256",
                }
            )
            output = exact_table(raw_output, keys, output_name)
            role = nonempty_string(output.get("role"), f"{output_name} role")
            if role in roles:
                fail(f"{output_name} role must be unique")
            roles.add(role)
            relative = relative_value(output.get("path"), f"{output_name} path")
            if relative in paths:
                fail(f"asset output path is duplicated: {relative}")
            paths.add(relative)
            _sha256(output.get("sha256"), f"{output_name} output")
            if kind == "7z":
                relative_value(output.get("member"), f"{output_name} member")
            normalized_outputs.append(output)
        result.append({**source, "output": normalized_outputs})
    return result


def asset_bundle_paths(path: Path) -> dict[str, str]:
    """Derive bundle paths solely from the selected asset-lock outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in load_asset_lock(path)
        for output in source["output"]
    }


def discover_targets() -> tuple[str, ...]:
    """Discover data-only target manifests without a central registry."""
    root = ROOT / "targets"
    targets = tuple(
        path.name
        for path in sorted(root.iterdir())
        if path.is_dir()
        and not path.is_symlink()
        and TARGET_NAME.fullmatch(path.name) is not None
        and (path / "target.toml").is_file()
        and not (path / "target.toml").is_symlink()
    )
    if not targets:
        fail("no targets are defined")
    return targets


def target_directory(target: str) -> Path:
    """Return one validated target directory in the fixed repository layout."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    path = ROOT / "targets" / target
    if path.is_symlink() or not path.is_dir():
        fail(f"unknown target: {target}")
    return path


def target_release_manifest_path(target: str) -> Path:
    """Return the fixed release-manifest path for one target."""
    return target_directory(target) / "release/manifest.toml"


def target_asset_lock_path(target: str) -> Path:
    """Return the fixed loader asset-lock path for one target."""
    return target_directory(target) / "loader/assets.lock.toml"


def target_defconfig_path(target: str) -> Path:
    """Return the fixed kernel defconfig path for one target."""
    return target_directory(target) / "kernel/defconfig"


def profiles_directory(target: str) -> Path:
    """Return the optional, target-owned root for build profiles."""
    path = target_directory(target) / "profiles"
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"target profiles directory is invalid: {path}")
    return path


def profile_directory(target: str, profile: str) -> Path:
    """Return one validated target-owned profile directory."""
    if not isinstance(profile, str) or TARGET_NAME.fullmatch(profile) is None:
        fail(f"invalid profile name: {profile}")
    path = profiles_directory(target) / profile
    if path.is_symlink() or not path.is_dir():
        fail(f"unknown profile: {target}/{profile}")
    return path


def profile_manifest_path(target: str, profile: str) -> Path:
    """Return the fixed manifest path for one target profile."""
    path = profile_directory(target, profile) / "profile.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"profile manifest is missing or invalid: {path}")
    return path


def profile_source_path(target: str, profile: str, relative: str) -> Path:
    """Resolve one regular profile source without following a symlink."""
    source = relative_value(relative, "profile source")
    root = profile_directory(target, profile)
    path = root
    for part in Path(source).parts:
        path /= part
        if path.is_symlink():
            fail(f"profile source must not be a symlink: {path}")
    if not path.is_file():
        fail(f"profile source is missing or invalid: {path}")
    return path


def profile_source_directory_path(target: str, profile: str, relative: str) -> Path:
    """Resolve one complete profile-owned source directory without symlinks."""
    source = relative_value(relative, "profile source directory")
    root = profile_directory(target, profile)
    path = root
    for part in Path(source).parts:
        path /= part
        if path.is_symlink():
            fail(f"profile source directory must not contain a symlink: {path}")
    if not path.is_dir():
        fail(f"profile source directory is missing or invalid: {path}")
    for child in path.rglob("*"):
        if child.is_symlink():
            fail(f"profile source directory must not contain a symlink: {child}")
    return path


def discover_profiles(target: str) -> tuple[str, ...]:
    """Discover only complete, non-symlinked profile definitions for one target."""
    root = profiles_directory(target)
    if not root.exists():
        return ()
    profiles: list[str] = []
    for path in sorted(root.iterdir()):
        if path.is_symlink() or not path.is_dir():
            fail(f"target profile entry is invalid: {path}")
        if TARGET_NAME.fullmatch(path.name) is None:
            fail(f"invalid profile name: {path.name}")
        manifest = path / "profile.toml"
        if manifest.is_symlink() or not manifest.is_file():
            fail(f"profile manifest is missing or invalid: {manifest}")
        profiles.append(path.name)
    return tuple(profiles)


def _profile_relative_source(profile: str, source: str) -> str:
    """Project one profile-relative source into the target-owned source tree."""
    return f"profiles/{profile}/{relative_value(source, 'profile source')}"


def _profile_linux_sources(target: str, profile: str, linux: dict[str, Any]) -> None:
    """Require every profile projection input to be a direct regular file."""
    for relative in linux["patches"]:
        profile_source_path(target, profile, relative)
    for key in ("copies", "appends"):
        for step in linux[key]:
            profile_source_path(target, profile, step["source"])


def _profile_steps(profile: str, steps: list[dict[str, str]]) -> list[dict[str, str]]:
    """Turn profile-local projection inputs into target-relative inputs."""
    return [
        {
            "source": _profile_relative_source(profile, step["source"]),
            "destination": step["destination"],
        }
        for step in steps
    ]


def _reject_duplicate_steps(steps: list[dict[str, str]], name: str) -> None:
    """Reject an operation that would apply the same projection more than once."""
    operations = {(step["source"], step["destination"]) for step in steps}
    if len(operations) != len(steps):
        fail(f"{name} must not contain duplicate operations")


def _profile_linux_root(value: object, name: str) -> dict[str, Any]:
    """Validate the root filesystem contract consumed by one profile kernel."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    kind = value.get("kind")
    if kind == "initramfs":
        exact_table(value, {"kind"}, name)
        return {"kind": "initramfs"}
    if kind != "external":
        fail(f"{name} kind must be initramfs or external")
    root = exact_table(value, {"kind", "filesystem", "wait_seconds"}, name)
    if root.get("filesystem") != "ext4":
        fail(f"{name} filesystem must be ext4")
    wait_seconds = integer_value(
        root.get("wait_seconds"),
        f"{name} wait_seconds",
        bounds=(1, 60),
    )
    return {
        "kind": "external",
        "filesystem": "ext4",
        "wait_seconds": wait_seconds,
    }


def _profile_bootstrap(target: str, profile: str, value: object, name: str) -> dict[str, str]:
    """Validate the selected resident bootstrap implementation."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    kind = value.get("kind")
    if kind == "linux":
        exact_table(value, {"kind"}, name)
        return {"kind": "linux"}
    if kind != "uboot-stage0":
        fail(f"{name} kind must be linux or uboot-stage0")
    bootstrap = exact_table(value, {"kind", "source", "image", "map"}, name)
    source = relative_value(bootstrap.get("source"), f"{name} source")
    profile_source_directory_path(target, profile, source)
    return {
        "kind": "uboot-stage0",
        "source": source,
        "image": relative_value(bootstrap.get("image"), f"{name} image"),
        "map": relative_value(bootstrap.get("map"), f"{name} map"),
    }


def _profile_uboot_lock(target: str, profile: str, relative: str) -> dict[str, str]:
    """Load one profile-owned immutable U-Boot source lock."""
    path = profile_source_path(target, profile, relative)
    try:
        with path.open("rb") as stream:
            raw = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"profile {profile} U-Boot lock is invalid: {path}: {error}")
    lock = exact_table(
        raw,
        {
            "version",
            "repository",
            "tag",
            "commit",
            "archive_url",
            "archive_sha256",
            "license",
        },
        f"profile {profile} U-Boot lock",
    )
    normalized: dict[str, Any] = {
        key: nonempty_string(lock.get(key), f"profile {profile} U-Boot lock {key}") for key in lock
    }
    if re.fullmatch(r"[0-9]{4}\.[0-9]{2}", normalized["version"]) is None:
        fail(f"profile {profile} U-Boot version must use YYYY.MM syntax")
    if not normalized["repository"].startswith("https://"):
        fail(f"profile {profile} U-Boot repository must use HTTPS")
    if not normalized["archive_url"].startswith("https://"):
        fail(f"profile {profile} U-Boot archive_url must use HTTPS")
    if GIT_COMMIT.fullmatch(normalized["commit"]) is None:
        fail(f"profile {profile} U-Boot commit must be 40 lowercase hex digits")
    _sha256(normalized["archive_sha256"], f"profile {profile} U-Boot archive")
    if normalized["tag"] != f"v{normalized['version']}":
        fail(f"profile {profile} U-Boot tag must match its version")
    return normalized


def _profile_uboot(target: str, profile: str, value: object, name: str) -> dict[str, Any]:
    """Validate the U-Boot capability actually implemented by the build."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    kind = value.get("kind")
    if kind == "none":
        exact_table(value, {"kind"}, name)
        return {"kind": "none"}
    if kind != "full":
        fail(f"{name} kind must be none or full")
    uboot = exact_table(
        value,
        {"kind", "source", "archive_prefix", "defconfig", "patches", "copies"},
        name,
    )
    source = relative_value(uboot.get("source"), f"{name} source")
    archive_prefix = relative_value(uboot.get("archive_prefix"), f"{name} archive_prefix")
    defconfig = relative_value(uboot.get("defconfig"), f"{name} defconfig")
    patches = path_array(uboot.get("patches"), f"{name} patches", allow_empty=True)
    copies = path_steps(uboot.get("copies"), f"{name} copies")
    profile_source_path(target, profile, defconfig)
    for relative in patches:
        profile_source_path(target, profile, relative)
    for step in copies:
        copy_source = ROOT / step["source"]
        if copy_source.is_symlink() or not copy_source.is_file():
            fail(f"{name} copy source is missing or invalid: {copy_source}")
    normalized: dict[str, Any] = {
        "kind": "full",
        "source": source,
        "archive_prefix": archive_prefix,
        "lock": _profile_uboot_lock(target, profile, source),
        "defconfig": defconfig,
        "patches": patches,
        "copies": copies,
    }
    return normalized


def _profile_layout(value: object, name: str, platform_layout: dict[str, int]) -> dict[str, int]:
    """Merge and validate one profile delta over the platform boot layout."""
    fields = {
        "resident_start",
        "resident_limit",
        "uboot_load",
        "uboot_size",
        "uboot_stack",
        "fit_load",
        "fit_size",
        "fdt_pad",
    }
    layout = exact_table(value, fields, name)
    normalized = {
        **platform_layout,
        **{
            field: integer_value(
                layout.get(field),
                f"{name} {field}",
                bounds=(0, 0xFFFFFFFF),
                alignment=0x1000,
            )
            for field in fields
        },
    }
    ram_end = normalized["ram_base"] + normalized["ram_size"]
    kernel_end = normalized["kernel_load"] + normalized["kernel_size"]
    fit_end = normalized["fit_load"] + normalized["fit_size"]
    fdt_end = normalized["fdt_load"] + normalized["fdt_size"]
    framebuffer_end = normalized["framebuffer"] + normalized["framebuffer_size"]
    if ram_end > 0x100000000:
        fail(f"{name} RAM range exceeds the 32-bit address space")
    if not (
        normalized["ram_base"]
        <= normalized["resident_start"]
        < normalized["uboot_stack"]
        < normalized["resident_limit"]
        == normalized["uboot_load"]
    ):
        fail(f"{name} resident stage, stack and U-Boot load are inconsistent")
    if normalized["uboot_load"] + normalized["uboot_size"] > normalized["kernel_load"]:
        fail(f"{name} U-Boot binary arena overlaps the kernel arena")
    if not (
        normalized["kernel_load"]
        <= normalized["kernel_entry"]
        < kernel_end
        == normalized["fit_load"]
    ):
        fail(f"{name} kernel entry or FIT boundary is inconsistent")
    if fit_end != normalized["fdt_load"]:
        fail(f"{name} FIT arena must end at the fixed DTB address")
    if normalized["fdt_pad"] >= normalized["fdt_size"]:
        fail(f"{name} U-Boot FDT padding must fit inside the DTB arena")
    if fdt_end > normalized["framebuffer"]:
        fail(f"{name} DTB arena overlaps the framebuffer")
    if framebuffer_end != ram_end:
        fail(f"{name} framebuffer must end at the RAM boundary")
    return normalized


def _platform_boot_layout(value: object, name: str) -> dict[str, int]:
    """Validate the volatile Linux handoff layout shared by one platform."""
    fields = {
        "ram_base",
        "ram_size",
        "timer_hz",
        "kernel_load",
        "kernel_entry",
        "kernel_size",
        "fdt_load",
        "fdt_size",
        "framebuffer",
        "framebuffer_size",
    }
    layout = exact_table(value, fields, name)
    normalized = {
        field: integer_value(
            layout.get(field),
            f"{name} {field}",
            bounds=(0, 0xFFFFFFFF),
            alignment=1 if field == "timer_hz" else (4 if field == "kernel_entry" else 0x1000),
        )
        for field in fields
    }
    ram_end = normalized["ram_base"] + normalized["ram_size"]
    kernel_end = normalized["kernel_load"] + normalized["kernel_size"]
    fdt_end = normalized["fdt_load"] + normalized["fdt_size"]
    framebuffer_end = normalized["framebuffer"] + normalized["framebuffer_size"]
    if ram_end > 0x100000000:
        fail(f"{name} RAM range exceeds the 32-bit address space")
    if not 1 <= normalized["timer_hz"] <= 1000000:
        fail(f"{name} timer_hz is outside the supported range")
    if not (
        normalized["kernel_load"]
        <= normalized["kernel_entry"]
        < kernel_end
        <= normalized["fdt_load"]
    ):
        fail(f"{name} kernel entry or DTB boundary is inconsistent")
    if fdt_end > normalized["framebuffer"] or framebuffer_end != ram_end:
        fail(f"{name} DTB/framebuffer ranges are inconsistent")
    return normalized


def _profile_fit(value: object, name: str, layout: dict[str, int] | None) -> dict[str, Any]:
    """Validate one native FIT image contract."""
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    kind = value.get("kind")
    if kind == "none":
        exact_table(value, {"kind"}, name)
        return {"kind": "none"}
    if kind != "sha256":
        fail(f"{name} kind must be none or sha256")
    if layout is None:
        fail(f"{name} SHA-256 FIT requires a boot layout")
    fit = exact_table(
        value,
        {"kind", "filename"},
        name,
    )
    return {
        "kind": "sha256",
        "filename": basename_value(fit.get("filename"), f"{name} filename"),
        "kernel_load": layout["kernel_load"],
        "kernel_entry": layout["kernel_entry"],
        "fdt_load": layout["fdt_load"],
    }


def _profile_storage(value: object, name: str) -> dict[str, Any]:
    """Validate one fixed MBR/FAT/ext4 removable-media layout."""
    storage = exact_table(
        value,
        {
            "filename",
            "disk_signature",
            "boot_partition",
            "boot_offset",
            "boot_size",
            "boot_label",
            "root_partition",
            "root_offset",
            "root_size",
            "root_filename",
            "root_label",
            "root_uuid",
            "block_size",
            "inode_size",
        },
        name,
    )
    disk_signature = integer_value(
        storage.get("disk_signature"), f"{name} disk_signature", bounds=(1, 0xFFFFFFFF)
    )
    boot_partition = integer_value(
        storage.get("boot_partition"), f"{name} boot_partition", bounds=(1, 4)
    )
    root_partition = integer_value(
        storage.get("root_partition"), f"{name} root_partition", bounds=(1, 4)
    )
    if (boot_partition, root_partition) != (1, 2):
        fail(f"{name} must use boot partition 1 followed by root partition 2")
    boot_offset = integer_value(
        storage.get("boot_offset"),
        f"{name} boot_offset",
        bounds=(1024 * 1024, 0xFFFFFFFF),
        alignment=1024 * 1024,
    )
    boot_size = integer_value(
        storage.get("boot_size"),
        f"{name} boot_size",
        bounds=(16 * 1024 * 1024, 4 * 1024 * 1024 * 1024),
        alignment=1024 * 1024,
    )
    root_offset = integer_value(
        storage.get("root_offset"),
        f"{name} root_offset",
        bounds=(1024 * 1024, 0xFFFFFFFF),
        alignment=1024 * 1024,
    )
    if root_offset != boot_offset + boot_size:
        fail(f"{name} root partition must immediately follow the boot partition")
    root_size = integer_value(
        storage.get("root_size"),
        f"{name} root_size",
        bounds=(16 * 1024 * 1024, 4 * 1024 * 1024 * 1024),
        alignment=1024 * 1024,
    )
    if root_offset + root_size > 0x100000000:
        fail(f"{name} image exceeds the 32-bit MBR addressable range")
    boot_label = nonempty_string(storage.get("boot_label"), f"{name} boot_label")
    if len(boot_label) > 11 or not boot_label.isascii():
        fail(f"{name} boot_label must be at most 11 ASCII characters")
    root_label = nonempty_string(storage.get("root_label"), f"{name} root_label")
    if len(root_label) > 16 or not root_label.isascii():
        fail(f"{name} root_label must be at most 16 ASCII characters")
    filesystem_uuid = nonempty_string(storage.get("root_uuid"), f"{name} root_uuid")
    if UUID.fullmatch(filesystem_uuid) is None:
        fail(f"{name} root_uuid must use canonical lowercase UUID syntax")
    block_size = integer_value(
        storage.get("block_size"), f"{name} block_size", bounds=(1024, 4096)
    )
    if block_size not in {1024, 2048, 4096}:
        fail(f"{name} block_size must be 1024, 2048 or 4096")
    inode_size = integer_value(storage.get("inode_size"), f"{name} inode_size", bounds=(128, 512))
    if inode_size not in {128, 256, 512}:
        fail(f"{name} inode_size must be 128, 256 or 512")
    partuuid = f"{disk_signature:08x}-{root_partition:02x}"
    return {
        "filename": basename_value(storage.get("filename"), f"{name} filename"),
        "disk_signature": disk_signature,
        "boot_partition": boot_partition,
        "boot_offset": boot_offset,
        "boot_size": boot_size,
        "boot_label": boot_label,
        "root_partition": root_partition,
        "root_offset": root_offset,
        "root_size": root_size,
        "root_filename": basename_value(storage.get("root_filename"), f"{name} root_filename"),
        "root_label": root_label,
        "root_uuid": filesystem_uuid,
        "partuuid": partuuid,
        "block_size": block_size,
        "inode_size": inode_size,
    }


def load_profile(target: str, profile: str, platform_layout: dict[str, int]) -> dict[str, Any]:
    """Load one exact target-owned build profile without applying it."""
    path = profile_manifest_path(target, profile)
    try:
        with path.open("rb") as stream:
            raw = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"profile manifest is invalid: {path}: {error}")
    required_fields = {
        "name",
        "linux",
        "rootfs",
        "bootstrap",
        "uboot",
        "fit",
        "runtime",
    }
    optional_fields = {"layout", "storage"}
    if (
        not isinstance(raw, dict)
        or not required_fields.issubset(raw)
        or set(raw) - required_fields - optional_fields
    ):
        fail(
            f"profile {profile} must contain exactly the required profile tables "
            "and optional layout/storage"
        )
    config = raw
    if config.get("name") != profile:
        fail(f"profile name does not match its directory: {path}")

    linux = exact_table(
        config.get("linux"),
        {"config_enable", "config_disable", "patches", "copies", "appends", "root"},
        f"profile {profile} linux",
    )
    config_enable = kconfig_symbol_array(
        linux.get("config_enable"), f"profile {profile} linux config_enable"
    )
    config_disable = kconfig_symbol_array(
        linux.get("config_disable"), f"profile {profile} linux config_disable"
    )
    overlap = set(config_enable) & set(config_disable)
    if overlap:
        fail(
            f"profile {profile} linux config_enable/config_disable conflict: "
            + ", ".join(sorted(overlap))
        )
    root = _profile_linux_root(linux.get("root"), f"profile {profile} linux root")
    root_owned_symbols = {"CONFIG_BLK_DEV_INITRD", "CONFIG_EXT4_FS"}
    duplicated_root_symbols = root_owned_symbols & (set(config_enable) | set(config_disable))
    if duplicated_root_symbols:
        fail(
            f"profile {profile} linux root owns its Kconfig symbols: "
            + ", ".join(sorted(duplicated_root_symbols))
        )
    if root["kind"] == "external":
        config_enable.append("CONFIG_EXT4_FS")
        config_disable.append("CONFIG_BLK_DEV_INITRD")
    patches = path_array(
        linux.get("patches"), f"profile {profile} linux patches", allow_empty=True
    )
    copies = path_steps(linux.get("copies"), f"profile {profile} linux copies")
    appends = path_steps(linux.get("appends"), f"profile {profile} linux appends")
    _reject_duplicate_steps(copies, f"profile {profile} linux copies")
    _reject_duplicate_steps(appends, f"profile {profile} linux appends")
    copy_destinations = [step["destination"] for step in copies]
    if len(copy_destinations) != len(set(copy_destinations)):
        fail(f"profile {profile} linux copies must not target one destination twice")
    normalized_linux = {
        "config_enable": config_enable,
        "config_disable": config_disable,
        "patches": patches,
        "copies": copies,
        "appends": appends,
        "root": root,
    }
    _profile_linux_sources(target, profile, normalized_linux)

    rootfs = exact_table(
        config.get("rootfs"),
        {"packages", "exclude_packages"},
        f"profile {profile} rootfs",
    )
    packages = package_array(rootfs.get("packages"), f"profile {profile} rootfs packages")
    exclude_packages = package_array(
        rootfs.get("exclude_packages"), f"profile {profile} rootfs exclude_packages"
    )
    package_overlap = set(packages) & set(exclude_packages)
    if package_overlap:
        fail(
            f"profile {profile} rootfs packages/exclude_packages conflict: "
            + ", ".join(sorted(package_overlap))
        )

    runtime = exact_table(
        config.get("runtime"),
        {"transport", "runnable"},
        f"profile {profile} runtime",
    )
    transport = runtime.get("transport")
    if transport not in {"usb-ncm", "none"}:
        fail(f"profile {profile} runtime transport must be usb-ncm or none")
    runnable = runtime.get("runnable")
    if type(runnable) is not bool:
        fail(f"profile {profile} runtime runnable must be a boolean")
    bootstrap = _profile_bootstrap(
        target, profile, config.get("bootstrap"), f"profile {profile} bootstrap"
    )
    uboot = _profile_uboot(target, profile, config.get("uboot"), f"profile {profile} uboot")
    layout = (
        _profile_layout(config.get("layout"), f"profile {profile} layout", platform_layout)
        if "layout" in config
        else None
    )
    fit = _profile_fit(config.get("fit"), f"profile {profile} fit", layout)
    storage = (
        _profile_storage(config.get("storage"), f"profile {profile} storage")
        if "storage" in config
        else None
    )
    image = (
        {
            "kind": "ext4-root",
            "filename": storage["root_filename"],
            "partuuid": storage["partuuid"],
            "label": storage["root_label"],
            "uuid": storage["root_uuid"],
            "size": storage["root_size"],
            "block_size": storage["block_size"],
            "inode_size": storage["inode_size"],
        }
        if storage is not None
        else {"kind": "none"}
    )
    if fit["kind"] == "sha256" and uboot["kind"] != "full":
        fail(f"profile {profile} SHA-256 FIT requires full U-Boot")
    if uboot["kind"] == "full" and fit["kind"] != "sha256":
        fail(f"profile {profile} full U-Boot requires a SHA-256 FIT")
    if (bootstrap["kind"] == "uboot-stage0") != (uboot["kind"] == "full"):
        fail(f"profile {profile} resident U-Boot stage requires full U-Boot")
    if root["kind"] == "external":
        if layout is None or storage is None:
            fail(f"profile {profile} external root requires layout and storage")
        if image["kind"] != "ext4-root":
            fail(f"profile {profile} external root requires an ext4-root image")
        normalized_linux["root"] = {**root, "partuuid": image["partuuid"]}
    elif layout is not None or storage is not None:
        fail(f"profile {profile} layout/storage requires an external root")

    return {
        "name": profile,
        "linux": normalized_linux,
        "rootfs": {"packages": packages, "exclude_packages": exclude_packages},
        "bootstrap": bootstrap,
        "uboot": uboot,
        "fit": fit,
        "layout": layout,
        "storage": storage,
        "image": image,
        "runtime": {
            "transport": transport,
            "runnable": runnable,
        },
    }


def _validate_profile_rootfs_ownership(
    profile: str, profile_rootfs: dict[str, Any], platform: dict[str, Any]
) -> None:
    """Reject profile rootfs changes that do not describe one base-package delta."""
    owned = set(COMMON_PACKAGES) | set(platform["rootfs"]["packages"])
    additions = set(profile_rootfs["packages"])
    excludes = set(profile_rootfs["exclude_packages"])
    unknown_excludes = excludes - owned
    if unknown_excludes:
        fail(
            f"profile {profile} rootfs excludes a package not owned by common/platform: "
            + ", ".join(sorted(unknown_excludes))
        )
    duplicate_additions = additions & owned
    if duplicate_additions:
        fail(
            f"profile {profile} rootfs packages duplicate common/platform ownership: "
            + ", ".join(sorted(duplicate_additions))
        )


def load_target(target: str, profile: str | None = None) -> dict[str, Any]:
    """Load one target definition and, when selected, apply one exact profile."""
    path = target_directory(target) / "target.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown target: {target}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {
            "identity",
            "platform",
            "bundle",
            "linux",
            "bootstrap",
            "adapter",
        },
        f"target {target}",
    )
    try:
        identity = validate_target_identity(config.get("identity"), f"target {target} identity")
    except IdentityError as error:
        fail(str(error))
    config["identity"] = identity
    platform_name = nonempty_string(config.get("platform"), f"target {target} platform")
    if VALUE_NAME.fullmatch(platform_name) is None:
        fail(f"target {target} has invalid platform: {path}")
    bundle = exact_table(config.get("bundle"), {"packages"}, "target bundle")
    package_array(bundle.get("packages"), "target bundle packages")

    linux = exact_table(
        config.get("linux"),
        {
            "dtb",
            "debug_dtb",
            "patches",
            "copies",
            "appends",
            "forbidden_config",
            "forbidden_dtb_markers",
        },
        "target linux",
    )
    relative_value(linux.get("dtb"), "target linux dtb")
    relative_value(linux.get("debug_dtb"), "target linux debug_dtb")
    path_array(linux.get("patches"), "target linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "target linux copies")
    path_steps(linux.get("appends"), "target linux appends")
    string_array(linux.get("forbidden_config"), "target linux forbidden_config")
    string_array(linux.get("forbidden_dtb_markers"), "target linux forbidden_dtb_markers")

    bootstrap = exact_table(
        config.get("bootstrap"),
        {
            "image",
            "map",
            "dtb_destination",
            "record_prefix",
        },
        "target bootstrap",
    )
    for key in ("image", "map", "dtb_destination"):
        relative_value(bootstrap.get(key), f"target bootstrap {key}")
    try:
        record_prefix = validate_record_prefix(bootstrap.get("record_prefix"))
    except IdentityError as error:
        fail(str(error))

    adapter = exact_table(
        config.get("adapter"),
        {
            "spi_mode",
            "lcd_id",
            "exec_distance",
            "backlight_channels",
            "backlight_level",
            "session_name",
            "boot_instructions",
        },
        "target adapter",
    )
    integer_value(adapter.get("spi_mode"), "target adapter spi_mode", bounds=(0, 3))
    integer_value(adapter.get("lcd_id"), "target adapter lcd_id", bounds=(0, 0xFFFFFFFF))
    integer_value(
        adapter.get("exec_distance"),
        "target adapter exec_distance",
        bounds=(0, 0xFFFF),
    )
    nonempty_string(adapter.get("backlight_channels"), "target adapter backlight_channels")
    integer_value(
        adapter.get("backlight_level"),
        "target adapter backlight_level",
        bounds=(0, 0x3F),
    )
    for key in ("session_name", "boot_instructions"):
        nonempty_string(adapter.get(key), f"target adapter {key}")

    platform = load_platform(str(config["platform"]))
    selected_profile: dict[str, Any] | None = None
    if profile is not None:
        selected_profile = load_profile(target, profile, platform["bootstrap"]["layout"])

    if identity["compatible"] == platform["identity"]["compatible"]:
        fail("target and platform compatibles must be distinct")
    if selected_profile is not None:
        _validate_profile_rootfs_ownership(str(profile), selected_profile["rootfs"], platform)
    profile_linux = (
        selected_profile["linux"]
        if selected_profile is not None
        else {
            "config_enable": [],
            "config_disable": [],
            "patches": [],
            "copies": [],
            "appends": [],
            "root": {"kind": "initramfs"},
        }
    )
    copied_destinations = {
        step["destination"] for step in [*platform["linux"]["copies"], *linux["copies"]]
    }
    profile_copy_destinations = {step["destination"] for step in profile_linux["copies"]}
    copy_conflicts = copied_destinations & profile_copy_destinations
    if copy_conflicts:
        fail(
            f"profile {profile} linux copies conflict with an existing copy destination: "
            + ", ".join(sorted(copy_conflicts))
        )
    config["linux"] = {
        **linux,
        "config_enable": profile_linux["config_enable"],
        "config_disable": profile_linux["config_disable"],
        "patches": [
            *linux["patches"],
            *[
                _profile_relative_source(str(profile), source)
                for source in profile_linux["patches"]
            ],
        ],
        "copies": [*linux["copies"], *_profile_steps(str(profile), profile_linux["copies"])],
        "appends": [
            *linux["appends"],
            *_profile_steps(str(profile), profile_linux["appends"]),
        ],
        "root": profile_linux["root"],
    }
    config["rootfs"] = (
        selected_profile["rootfs"]
        if selected_profile is not None
        else {"packages": [], "exclude_packages": []}
    )
    config["profile"] = profile
    platform_bootstrap = platform["bootstrap"]
    platform_runtime = platform["runtime"]
    profile_bootstrap = selected_profile["bootstrap"] if selected_profile is not None else None
    bootstrap_kind = profile_bootstrap["kind"] if profile_bootstrap is not None else "linux"
    profile_layout = selected_profile["layout"] if selected_profile is not None else None
    stage0_layout = profile_layout if isinstance(profile_layout, dict) else None
    if bootstrap_kind == "uboot-stage0":
        if stage0_layout is None:
            fail(f"profile {profile} resident U-Boot stage has no layout")
        if stage0_layout["resident_start"] != platform_bootstrap["load_address"]:
            fail(f"profile {profile} resident stage must start at the platform load address")
    config["bootstrap"] = {
        "kind": bootstrap_kind,
        "source": (
            _profile_relative_source(str(profile), profile_bootstrap["source"])
            if profile_bootstrap is not None and bootstrap_kind == "uboot-stage0"
            else "bootstrap"
        ),
        "image": (
            profile_bootstrap["image"]
            if profile_bootstrap is not None and bootstrap_kind == "uboot-stage0"
            else bootstrap["image"]
        ),
        "map": (
            profile_bootstrap["map"]
            if profile_bootstrap is not None and bootstrap_kind == "uboot-stage0"
            else bootstrap["map"]
        ),
        "record_prefix": record_prefix,
        "kernel_destination": platform_bootstrap["kernel_destination"],
        "dtb_destination": bootstrap["dtb_destination"],
        "load_address": (
            stage0_layout["resident_start"]
            if stage0_layout is not None
            else platform_bootstrap["load_address"]
        ),
        "payload_limit": (
            stage0_layout["resident_limit"]
            if stage0_layout is not None
            else platform_bootstrap["payload_limit"]
        ),
        "toolchain": platform_bootstrap["toolchain"],
        "lto": platform_bootstrap["lto"],
    }
    config["uboot"] = (
        selected_profile["uboot"] if selected_profile is not None else {"kind": "none"}
    )
    config["fit"] = selected_profile["fit"] if selected_profile is not None else {"kind": "none"}
    config["layout"] = selected_profile["layout"] if selected_profile is not None else None
    config["storage"] = selected_profile["storage"] if selected_profile is not None else None
    config["image"] = (
        selected_profile["image"] if selected_profile is not None else {"kind": "none"}
    )
    transport = "usb-ncm"
    if selected_profile is not None:
        transport = selected_profile["runtime"]["transport"]
    config["runtime"] = {
        "fdl1_load_address": platform_runtime["fdl1_load_address"],
        "assets": asset_bundle_paths(target_asset_lock_path(target)),
        "adapter": {
            **platform_runtime["adapter"],
            **adapter,
        },
        "usb": platform_runtime["usb"],
        "transport": transport,
        "runnable": (
            selected_profile["runtime"]["runnable"] if selected_profile is not None else True
        ),
    }
    return config


def load_release(target: str) -> dict[str, Any]:
    """Load the exact data-only release manifest for one target."""
    path = target_release_manifest_path(target)
    if path.is_symlink() or not path.is_file():
        fail(f"target release manifest is missing or invalid: {path}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    manifest = exact_table(
        raw,
        {"image", "bundle_files", "runtime_files", "documents"},
        f"target {target} release manifest",
    )
    image = relative_value(manifest.get("image"), "release manifest image")
    bundle_files = path_array(manifest.get("bundle_files"), "release bundle_files")
    runtime_files = path_array(manifest.get("runtime_files"), "release runtime_files")
    documents = path_array(manifest.get("documents"), "release documents")
    if image not in runtime_files:
        fail("release manifest image must be a runtime file")
    if not set(runtime_files).issubset(bundle_files):
        fail("release runtime files must be bundle files")
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "documents": documents,
    }


def validate_host_tool(value: object, index: int) -> dict[str, Any]:
    """Validate one typed host build recipe."""
    name = f"platform host tools[{index}]"
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    recipe_type = value.get("type")
    if recipe_type == "make-archive":
        recipe = exact_table(
            value,
            {
                "type",
                "name",
                "source_lock",
                "cache_name",
                "archive_prefix",
                "source_directory",
                "binary",
                "link",
                "members",
                "copies",
                "patches",
                "self_test",
            },
            name,
        )
        for key in ("name", "source_lock", "cache_name", "archive_prefix", "binary"):
            nonempty_string(recipe.get(key), f"{name} {key}")
        if recipe.get("link") != "static-libusb":
            fail(f"{name} link must be static-libusb")
        relative_value(recipe.get("source_directory"), f"{name} source_directory")
        members = recipe.get("members")
        if not isinstance(members, list) or not members:
            fail(f"{name} members must be a non-empty array")
        for member_index, raw_member in enumerate(members):
            member = exact_table(
                raw_member,
                {"path", "digest_key"},
                f"{name} members[{member_index}]",
            )
            relative_value(member.get("path"), f"{name} member path")
            nonempty_string(member.get("digest_key"), f"{name} member digest_key")
        path_steps(recipe.get("copies"), f"{name} copies")
        path_array(recipe.get("patches"), f"{name} patches", allow_empty=True)
        if type(recipe.get("self_test")) is not bool:
            fail(f"{name} self_test must be a boolean")
        return recipe
    if recipe_type == "cc-libusb":
        recipe = exact_table(value, {"type", "name", "source", "self_test"}, name)
        nonempty_string(recipe.get("name"), f"{name} name")
        relative_value(recipe.get("source"), f"{name} source")
        if type(recipe.get("self_test")) is not bool:
            fail(f"{name} self_test must be a boolean")
        return recipe
    fail(f"{name} has unsupported type: {recipe_type}")
    return {}


def load_platform(platform: str) -> dict[str, Any]:
    """Load one exact reusable platform definition."""
    if TARGET_NAME.fullmatch(platform) is None:
        fail(f"invalid platform name: {platform}")
    path = ROOT / "platforms" / platform / "platform.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown platform: {platform}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {
            "identity",
            "rootfs",
            "bundle",
            "linux",
            "bootstrap",
            "runtime",
            "host",
        },
        f"platform {platform}",
    )
    try:
        config["identity"] = validate_platform_identity(
            config.get("identity"), f"platform {platform} identity"
        )
    except IdentityError as error:
        fail(str(error))

    rootfs = exact_table(config.get("rootfs"), {"packages"}, "platform rootfs")
    package_array(rootfs.get("packages"), "platform rootfs packages")
    bundle = exact_table(config.get("bundle"), {"packages"}, "platform bundle")
    package_array(bundle.get("packages"), "platform bundle packages")

    linux = exact_table(
        config.get("linux"),
        {
            "source_lock",
            "arch",
            "cross_compile",
            "analysis_cross_compile",
            "config_script",
            "image_output",
            "dtb_output_directory",
            "targets",
            "patches",
            "copies",
            "appends",
        },
        "platform linux",
    )
    for key in ("source_lock", "arch", "cross_compile", "analysis_cross_compile"):
        nonempty_string(linux.get(key), f"platform linux {key}")
    for key in ("config_script", "image_output", "dtb_output_directory"):
        relative_value(linux.get(key), f"platform linux {key}")
    string_array(linux.get("targets"), "platform linux targets")
    path_array(linux.get("patches"), "platform linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "platform linux copies")
    path_steps(linux.get("appends"), "platform linux appends")

    bootstrap = exact_table(
        config.get("bootstrap"),
        {
            "vendor_source_lock",
            "vendor_cache_name",
            "archive_prefix",
            "source_destination",
            "vendor_destination",
            "output_destination",
            "pack_reloc",
            "safety_target",
            "build_targets",
            "files",
            "shared_copies",
            "kernel_destination",
            "load_address",
            "payload_limit",
            "layout",
            "toolchain",
            "lto",
        },
        "platform bootstrap",
    )
    for key in ("vendor_source_lock", "vendor_cache_name", "archive_prefix", "safety_target"):
        nonempty_string(bootstrap.get(key), f"platform bootstrap {key}")
    for key in ("source_destination", "vendor_destination", "output_destination", "pack_reloc"):
        relative_value(bootstrap.get(key), f"platform bootstrap {key}")
    string_array(bootstrap.get("build_targets"), "platform bootstrap build_targets")
    path_array(bootstrap.get("files"), "platform bootstrap files")
    path_steps(bootstrap.get("shared_copies"), "platform bootstrap shared_copies")
    relative_value(bootstrap.get("kernel_destination"), "platform bootstrap kernel_destination")
    integer_value(
        bootstrap.get("load_address"),
        "platform bootstrap load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    integer_value(
        bootstrap.get("payload_limit"),
        "platform bootstrap payload_limit",
        bounds=(1, 0x100000000),
        alignment=4,
    )
    bootstrap["layout"] = _platform_boot_layout(
        bootstrap.get("layout"), "platform bootstrap layout"
    )
    nonempty_string(bootstrap.get("toolchain"), "platform bootstrap toolchain")
    integer_value(bootstrap.get("lto"), "platform bootstrap lto", bounds=(0, 1))

    runtime = exact_table(
        config.get("runtime"),
        {"fdl1_load_address", "adapter", "usb"},
        "platform runtime",
    )
    integer_value(
        runtime.get("fdl1_load_address"),
        "platform runtime fdl1_load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    adapter = exact_table(
        runtime.get("adapter"),
        {
            "brightness",
            "rotation",
            "handoff_wait_seconds",
            "usb_release_wait_seconds",
        },
        "platform runtime adapter",
    )
    integer_value(adapter.get("brightness"), "platform adapter brightness", bounds=(0, 100))
    integer_value(
        adapter.get("rotation"),
        "platform adapter rotation",
        bounds=(0, 270),
    )
    if adapter["rotation"] not in {0, 90, 180, 270}:
        fail("platform adapter rotation must be 0, 90, 180 or 270")
    integer_value(
        adapter.get("handoff_wait_seconds"),
        "platform adapter handoff_wait_seconds",
        bounds=(1, 3600),
    )
    integer_value(
        adapter.get("usb_release_wait_seconds"),
        "platform adapter usb_release_wait_seconds",
        bounds=(1, 300),
    )
    usb = exact_table(runtime.get("usb"), {"bootrom", "linux_gadget"}, "platform runtime usb")
    validate_usb(usb.get("bootrom"), "platform runtime bootrom USB")
    validate_usb(
        usb.get("linux_gadget"),
        "platform runtime linux_gadget USB",
        interface_fields=True,
    )

    host = exact_table(
        config.get("host"),
        {"runtime_tools", "tools"},
        "platform host",
    )
    tools = host.get("tools")
    if not isinstance(tools, list) or not tools:
        fail("platform host tools must be a non-empty array")
    validated_tools = [validate_host_tool(tool, index) for index, tool in enumerate(tools)]
    tool_names = [tool["name"] for tool in validated_tools]
    if len(tool_names) != len(set(tool_names)):
        fail("platform host tool names must be unique")
    runtime_tools = host.get("runtime_tools")
    if not isinstance(runtime_tools, dict) or not runtime_tools:
        fail("platform host runtime_tools must be a non-empty table")
    for role, tool_name in runtime_tools.items():
        nonempty_string(role, "platform host runtime role")
        if tool_name not in tool_names:
            fail(f"platform host runtime role {role} references an unknown tool")

    return config


def verified_runtime_digest(target: str) -> str | None:
    """Return the hardware-qualified runtime closure digest, if present."""
    path = ROOT / "releases.lock.toml"
    if not path.is_file():
        fail(f"release verification lock is missing: {path}")
    with path.open("rb") as stream:
        lock = tomllib.load(stream)
    digest = lock.get("verified", {}).get(target)
    if digest is None:
        return None
    if (
        not isinstance(digest, str)
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        fail(f"invalid verified runtime SHA256 for {target}: {path}")
    return digest


def validate_source_policy() -> None:
    """Enforce the single-image source policy."""
    ignored = {".cache", ".git"}
    containerfiles: list[Path] = []
    # Prune the generated trees before descending: they hold millions of
    # cached build paths that a plain rglob would stat one by one.
    for directory, subdirectories, names in os.walk(ROOT):
        subdirectories[:] = [name for name in subdirectories if name not in ignored]
        for name in names:
            if name not in {"Containerfile", "Dockerfile"}:
                continue
            relative = (Path(directory) / name).relative_to(ROOT)
            if name == "Dockerfile":
                fail(f"unexpected additional container recipe: {relative}")
            containerfiles.append(relative)
    containerfiles.sort()
    if containerfiles != [Path("Containerfile")]:
        fail("source must contain exactly one root Containerfile")
    instructions = [
        line.strip()
        for line in (ROOT / containerfiles[0]).read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    if from_instructions != ["FROM ${BASE_IMAGE}"] or instructions[0] != "ARG BASE_IMAGE":
        fail("Containerfile must use one lock-provided FROM ${BASE_IMAGE}")


def load_container_lock() -> dict[str, Any]:
    """Load the one pinned OCI build-environment lock."""
    validate_source_policy()
    path = ROOT / "container.lock.toml"
    with path.open("rb") as stream:
        lock = tomllib.load(stream)
    if set(lock) != {"oci"}:
        fail(f"container lock must contain exactly oci: {path}")
    oci = lock.get("oci")
    if not isinstance(oci, dict) or set(oci) != {
        "repository",
        "platform",
        "base",
        "base_created",
    }:
        fail(f"container lock must define exactly one OCI repository: {path}")
    repository = oci.get("repository")
    base = oci.get("base")
    if repository != "localhost/fplinux-build":
        fail(f"container repository must be localhost/fplinux-build: {path}")
    if not isinstance(base, str) or re.fullmatch(r"[^@\s]+@sha256:[0-9a-f]{64}", base) is None:
        fail(f"container base image must be digest-pinned: {path}")
    if oci.get("platform") != "linux/amd64":
        fail(f"unsupported container platform: {path}")
    return lock


class _Hash(Protocol):
    """Describe the incremental hash operation used by recipe encoders."""

    def update(self, data: bytes) -> None:
        """Add bytes to the digest state."""


def _length_prefixed(value: _Hash, data: bytes) -> None:
    """Add one unambiguous byte field to an incremental recipe digest."""
    value.update(len(data).to_bytes(8, "big"))
    value.update(data)


def _stable_regular_file(path: Path) -> tuple[bytes, int]:
    """Read one regular causal recipe input."""
    if path.is_symlink() or not path.is_file():
        fail(f"recipe input is missing or invalid: {path}")
    try:
        return path.read_bytes(), path.stat().st_mode & 0o777
    except OSError as error:
        fail(f"recipe input cannot be read: {path}: {error}")


def _exact_file_recipe(paths: list[Path], *, prefix: bytes = b"") -> str:
    """Hash exact logical paths, bytes and modes for one causal file closure."""
    value = hashlib.sha256()
    _length_prefixed(value, prefix)
    for path in paths:
        contents, mode = _stable_regular_file(path)
        relative = path.relative_to(ROOT).as_posix()
        _length_prefixed(value, relative.encode())
        _length_prefixed(value, contents)
        _length_prefixed(value, mode.to_bytes(2, "big"))
    return value.hexdigest()


def container_image_build_arguments(lock: dict[str, Any] | None = None) -> tuple[str, ...]:
    """Return exact causal Podman build arguments, excluding tag and recipe label."""
    if lock is None:
        lock = load_container_lock()
    oci = lock["oci"]
    return (
        "--platform",
        oci["platform"],
        "--file",
        "Containerfile",
        "--build-arg",
        f"BASE_IMAGE={oci['base']}",
    )


def container_image_recipe_digest(lock: dict[str, Any] | None = None) -> str:
    """Hash only exact context bytes, modes, paths and build arguments."""
    arguments = (*container_image_build_arguments(lock), ".")
    encoded_arguments = b"".join(
        len(argument.encode()).to_bytes(8, "big") + argument.encode() for argument in arguments
    )
    return _exact_file_recipe(
        [
            ROOT / ".containerignore",
            ROOT / "Containerfile",
            ROOT / "package.json",
            ROOT / "package-lock.json",
        ],
        prefix=encoded_arguments,
    )


def container_image_reference(
    lock: dict[str, Any] | None = None, recipe: str | None = None
) -> str:
    """Return the recipe-addressed local reference for one build image."""
    if lock is None:
        lock = load_container_lock()
    if recipe is None:
        recipe = container_image_recipe_digest(lock)
    if len(recipe) != 64 or any(character not in "0123456789abcdef" for character in recipe):
        fail("container image recipe must be a lowercase SHA-256")
    return f"{lock['oci']['repository']}:{recipe}"


def check_orchestration_recipe_digest(image_recipe: str | None = None) -> str:
    """Hash only the implementation that can change cached check results."""
    fixed = [
        ROOT / "scripts/fplinux_cli/checkreceipts.py",
        ROOT / "scripts/fplinux_cli/common.py",
        ROOT / "scripts/fplinux_cli/config.py",
        ROOT / "scripts/fplinux_cli/container.py",
        ROOT / "scripts/fplinux_cli/image_state.py",
        ROOT / "scripts/fplinux_cli/identity.py",
        ROOT / "scripts/fplinux_cli/identity_codegen.py",
        ROOT / "scripts/fplinux_cli/output.py",
        ROOT / "scripts/fplinux_cli/workspace.py",
    ]
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    if len(image_recipe) != 64 or any(
        character not in "0123456789abcdef" for character in image_recipe
    ):
        fail("container image recipe must be a lowercase SHA-256")
    return _exact_file_recipe(fixed, prefix=bytes.fromhex(image_recipe))
