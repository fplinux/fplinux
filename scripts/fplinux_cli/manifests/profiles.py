# SPDX-License-Identifier: GPL-2.0-only
"""Resolve shared boot policy against board-owned inputs."""

from __future__ import annotations

import re
import tomllib
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.paths import profile_manifest_path, target_directory
from fplinux_cli.manifests.values import (
    GIT_COMMIT,
    UUID,
    basename_value,
    exact_table,
    integer_value,
    kconfig_line_array,
    nonempty_string,
    package_array,
    path_array,
    path_steps,
    relative_value,
    sha256_value,
)


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


def _profile_bootstrap(target: str, value: object, board: object) -> dict[str, str]:
    """Select Linux or the board-declared resident U-Boot bootstrap."""
    selection = exact_table(value, {"kind"}, "profile bootstrap")
    kind = selection["kind"]
    if kind == "linux":
        return {"kind": "linux"}
    if kind != "uboot-stage0":
        fail("profile bootstrap kind must be linux or uboot-stage0")
    bootstrap = exact_table(board, {"source", "image", "map"}, "target microsd bootstrap")
    result = {"kind": kind}
    for key in ("source", "image", "map"):
        result[key] = relative_value(bootstrap.get(key), f"target microsd bootstrap {key}")
    source = target_directory(target) / result["source"]
    if source.is_symlink() or not source.is_dir():
        fail(f"target microSD bootstrap source is missing or invalid: {source}")
    return result


def _profile_uboot_lock(profile: str, relative: str) -> dict[str, str]:
    """Load the immutable U-Boot source lock declared by the platform."""
    path = common.ROOT / relative
    if path.is_symlink() or not path.is_file():
        fail(f"U-Boot source lock is missing or invalid: {path}")
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
    sha256_value(normalized["archive_sha256"], f"profile {profile} U-Boot archive")
    if normalized["tag"] != f"v{normalized['version']}":
        fail(f"profile {profile} U-Boot tag must match its version")
    return normalized


def _profile_uboot(
    target: str, profile: str, value: object, board: object, platform: dict[str, Any]
) -> dict[str, Any]:
    """Combine shared U-Boot sources with the selected board integration."""
    selection = exact_table(value, {"kind"}, "profile uboot")
    kind = selection["kind"]
    if kind == "none":
        return {"kind": "none"}
    if kind != "full":
        fail("profile uboot kind must be none or full")
    uboot = exact_table(board, {"defconfig", "patches", "copies"}, "target microsd uboot")
    base = exact_table(
        platform.get("uboot"),
        {"source", "archive_prefix", "patches", "required_config", "copies"},
        "platform uboot",
    )
    source = relative_value(base.get("source"), "platform uboot source")
    archive_prefix = relative_value(base.get("archive_prefix"), "platform uboot archive_prefix")
    required_config = kconfig_line_array(
        base.get("required_config"), "platform uboot required_config"
    )
    defconfig = relative_value(uboot.get("defconfig"), "target microsd uboot defconfig")
    patches = path_array(uboot.get("patches"), "target microsd uboot patches", allow_empty=True)
    patches = [
        *path_array(base.get("patches"), "platform uboot patches", allow_empty=True),
        *(f"targets/{target}/{path}" for path in patches),
    ]
    copies = [
        *path_steps(base.get("copies"), "platform uboot copies"),
        *path_steps(uboot.get("copies"), "target microsd uboot copies"),
    ]
    for path in (target_directory(target) / defconfig, *(common.ROOT / path for path in patches)):
        if path.is_symlink() or not path.is_file():
            fail(f"U-Boot source is missing or invalid: {path}")
    for step in copies:
        path = common.ROOT / step["source"]
        if path.is_symlink() or not path.is_file():
            fail(f"U-Boot copy source is missing or invalid: {path}")
    return {
        "kind": kind,
        "source": source,
        "archive_prefix": archive_prefix,
        "lock": _profile_uboot_lock(profile, source),
        "defconfig": defconfig,
        "patches": patches,
        "copies": copies,
        "required_config": required_config,
    }


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


def load_profile(
    target: str, profile: str, platform: dict[str, Any], microsd: dict[str, Any]
) -> dict[str, Any]:
    """Load one global boot policy and resolve its board-owned inputs."""
    path = profile_manifest_path(target, profile)
    try:
        with path.open("rb") as stream:
            raw = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"profile manifest is invalid: {path}: {error}")
    fields = {"name", "linux", "rootfs", "bootstrap", "uboot", "fit", "runtime"}
    if profile == "microsd-uboot":
        fields |= {"layout", "storage"}
    config = exact_table(raw, fields, f"profile {profile}")
    if config["name"] != profile:
        fail(f"profile name does not match its directory: {path}")
    linux = exact_table(config["linux"], {"root"}, f"profile {profile} linux")
    root = _profile_linux_root(linux["root"], f"profile {profile} linux root")
    expected_root = "external" if profile == "microsd-uboot" else "initramfs"
    if root["kind"] != expected_root:
        fail(f"profile {profile} requires {expected_root} root")
    rootfs = exact_table(config["rootfs"], {"packages"}, f"profile {profile} rootfs")
    packages = package_array(rootfs["packages"], f"profile {profile} rootfs packages")
    expected_packages = ["fplinux-microsd-root"] if profile == "microsd-uboot" else []
    if packages != expected_packages:
        fail(f"profile {profile} rootfs packages must describe only boot maintenance")
    runtime = exact_table(
        config["runtime"], {"transport", "runnable"}, f"profile {profile} runtime"
    )
    if runtime != {"transport": "usb-ncm", "runnable": True}:
        fail("both profiles require runnable usb-ncm transport")
    bootstrap = _profile_bootstrap(target, config["bootstrap"], microsd.get("bootstrap"))
    uboot = _profile_uboot(target, profile, config["uboot"], microsd.get("uboot"), platform)
    layout = (
        _profile_layout(
            config["layout"], f"profile {profile} layout", platform["bootstrap"]["layout"]
        )
        if "layout" in config
        else None
    )
    storage = (
        _profile_storage(config["storage"], f"profile {profile} storage")
        if "storage" in config
        else None
    )
    fit = _profile_fit(config["fit"], f"profile {profile} fit", layout)
    external = root["kind"] == "external"
    if (
        (bootstrap["kind"] == "uboot-stage0") != external
        or (uboot["kind"] == "full") != external
        or (fit["kind"] == "sha256") != external
    ):
        fail(f"profile {profile} boot components do not match its root filesystem")
    image: dict[str, Any] = {"kind": "none"}
    if storage is not None:
        root = {**root, "partuuid": storage["partuuid"]}
        image = {
            "kind": "ext4-root",
            "filename": storage["root_filename"],
            "partuuid": storage["partuuid"],
            "label": storage["root_label"],
            "uuid": storage["root_uuid"],
            "size": storage["root_size"],
            "block_size": storage["block_size"],
            "inode_size": storage["inode_size"],
        }
    if external:
        config_enable = [
            "CONFIG_EXT4_FS",
            "CONFIG_ZRAM_BACKEND_LZO",
            "CONFIG_ZRAM_DEF_COMP_LZORLE",
        ]
        config_disable = [
            "CONFIG_BLK_DEV_INITRD",
            "CONFIG_ZRAM_BACKEND_ZSTD",
            "CONFIG_ZRAM_DEF_COMP_ZSTD",
        ]
    else:
        config_enable = [
            "CONFIG_ZRAM_BACKEND_ZSTD",
            "CONFIG_ZRAM_DEF_COMP_ZSTD",
        ]
        config_disable = [
            "CONFIG_ZRAM_BACKEND_LZO",
            "CONFIG_ZRAM_DEF_COMP_LZORLE",
        ]
    return {
        "name": profile,
        "linux": {
            "config_enable": config_enable,
            "config_disable": config_disable,
            "patches": microsd["linux_patches"] if external else [],
            "root": root,
        },
        "rootfs": {"packages": packages},
        "bootstrap": bootstrap,
        "uboot": uboot,
        "fit": fit,
        "layout": layout,
        "storage": storage,
        "image": image,
        "runtime": runtime,
    }
