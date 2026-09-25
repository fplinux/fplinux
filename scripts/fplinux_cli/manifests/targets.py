# SPDX-License-Identifier: GPL-2.0-only
"""Combine one board with its platform and boot profile."""

from __future__ import annotations

import re
from typing import Any

from fplinux_cli.common import fail, load_toml
from fplinux_cli.identity import IdentityError, validate_target_identity
from fplinux_cli.identity_codegen import validate_record_prefix
from fplinux_cli.manifests.assets import asset_bundle_paths
from fplinux_cli.manifests.paths import normalize_profile, target_asset_lock_path, target_directory
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.profiles import load_profile
from fplinux_cli.manifests.values import (
    VALUE_NAME,
    basename_value,
    exact_table,
    firmware_array,
    integer_value,
    nonempty_string,
    package_array,
    path_array,
    path_steps,
    relative_value,
    string_array,
)


def load_target(target: str, profile: str | None = None) -> dict[str, Any]:
    """Combine shared boot policy with one board and platform configuration."""
    profile = normalize_profile(profile)
    path = target_directory(target) / "target.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown target: {target}")
    raw = load_toml(path)
    config = exact_table(
        raw,
        {
            "identity",
            "microsd",
            *({"device_data"} if "device_data" in raw else set()),
            *({"bluetooth"} if "bluetooth" in raw else set()),
            *({"audio_profile"} if "audio_profile" in raw else set()),
            *({"fm_radio"} if "fm_radio" in raw else set()),
            *({"nand"} if "nand" in raw else set()),
            "platform",
            "rootfs",
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
    if "nand" in config:
        nand = config["nand"]
        if not isinstance(nand, dict) or set(nand) not in (
            {"raw_device"},
            {"raw_device", "id", "raw_page_bytes"},
        ):
            fail("target nand must contain raw_device and optionally both id and raw_page_bytes")
        raw_device = nonempty_string(nand["raw_device"], "target nand raw_device")
        if re.fullmatch(r"/dev/[A-Za-z0-9][A-Za-z0-9._-]*", raw_device) is None:
            fail("target nand raw_device must name one device directly under /dev")
        if "id" in nand:
            integer_value(nand["id"], "target nand id", bounds=(0, 0xFFFF))
            integer_value(
                nand["raw_page_bytes"], "target nand raw_page_bytes", bounds=(1, 0xFFFFFFFF)
            )
    platform_name = nonempty_string(config.get("platform"), f"target {target} platform")
    if VALUE_NAME.fullmatch(platform_name) is None:
        fail(f"target {target} has invalid platform: {path}")
    bundle = exact_table(config.get("bundle"), {"packages"}, "target bundle")
    package_array(bundle.get("packages"), "target bundle packages")
    rootfs = exact_table(config.get("rootfs"), {"packages"}, "target rootfs")
    target_rootfs_packages = package_array(rootfs.get("packages"), "target rootfs packages")

    linux = exact_table(
        config.get("linux"),
        {
            "dtb",
            "config_fragment",
            "memory",
            "debug_dtb",
            "patches",
            "copies",
            "appends",
            "forbidden_config",
            "forbidden_dtb_markers",
        },
        "target linux",
    )
    relative_value(linux.get("config_fragment"), "target linux config_fragment")
    memory = exact_table(linux.get("memory"), {"base", "size"}, "target linux memory")
    for key in ("base", "size"):
        integer_value(
            memory[key], f"target linux memory {key}", bounds=(0, 0xFFFFFFFF), alignment=0x1000
        )
    if not memory["size"] or memory["base"] + memory["size"] > 0x100000000:
        fail("target linux memory range is invalid")
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

    # Without the loader display settings the RAM loader runs headless.
    display_keys = {"spi_mode", "lcd_id", "backlight_channels", "backlight_level"}
    raw_adapter = config.get("adapter")
    has_display = isinstance(raw_adapter, dict) and not display_keys.isdisjoint(raw_adapter)
    adapter = exact_table(
        raw_adapter,
        {
            *(display_keys if has_display else set()),
            "exec_distance",
            "session_name",
            "boot_instructions",
        },
        "target adapter",
    )
    if has_display:
        integer_value(adapter.get("spi_mode"), "target adapter spi_mode", bounds=(0, 3))
        integer_value(adapter.get("lcd_id"), "target adapter lcd_id", bounds=(0, 0xFFFFFFFF))
        nonempty_string(adapter.get("backlight_channels"), "target adapter backlight_channels")
        integer_value(
            adapter.get("backlight_level"),
            "target adapter backlight_level",
            bounds=(0, 0x3F),
        )
    integer_value(
        adapter.get("exec_distance"),
        "target adapter exec_distance",
        bounds=(0, 0xFFFF),
    )
    for key in ("session_name", "boot_instructions"):
        nonempty_string(adapter.get(key), f"target adapter {key}")

    platform = load_platform(str(config["platform"]))
    microsd = exact_table(
        config.get("microsd"), {"linux_patches", "bootstrap", "uboot"}, "target microsd"
    )
    path_array(microsd.get("linux_patches"), "target microsd linux_patches", allow_empty=True)
    selected_profile = load_profile(target, profile or "default", platform, microsd)
    physical = platform["bootstrap"]["layout"]
    if not (
        physical["ram_base"]
        <= memory["base"]
        < memory["base"] + memory["size"]
        <= physical["ram_base"] + physical["ram_size"]
    ):
        fail("target Linux memory must fit inside physical RAM")
    if identity["compatible"] == platform["identity"]["compatible"]:
        fail("target and platform compatibles must be distinct")
    profile_linux = selected_profile["linux"]
    config["linux"] = {
        **linux,
        "config_enable": profile_linux["config_enable"],
        "config_disable": profile_linux["config_disable"],
        "patches": [*linux["patches"], *profile_linux["patches"]],
        "root": profile_linux["root"],
    }
    device_data_groups: dict[str, list[dict[str, Any]]] = {}
    if "bluetooth" in config:
        bluetooth = exact_table(config.pop("bluetooth"), {"firmware"}, "target bluetooth")
        device_data_groups["bluetooth"] = firmware_array(
            bluetooth["firmware"],
            "target bluetooth firmware",
        )
    if "audio_profile" in config:
        audio_profile = exact_table(
            config.pop("audio_profile"),
            {"firmware"},
            "target audio-profile",
        )
        device_data_groups["audio-profile"] = firmware_array(
            audio_profile["firmware"],
            "target audio-profile firmware",
        )
    if "fm_radio" in config:
        fm_radio = exact_table(config.pop("fm_radio"), {"firmware"}, "target FM radio")
        device_data_groups["fm-radio"] = firmware_array(
            fm_radio["firmware"],
            "target FM radio firmware",
        )
    if device_data_groups:
        device_data = exact_table(
            config.get("device_data"),
            {"parser"},
            "target device_data",
        )
        parser = basename_value(device_data["parser"], "target device_data parser")
        if not parser.endswith(".py"):
            fail("target device_data parser must be a Python filename")
        config["device_data"] = {"parser": parser, "groups": device_data_groups}
    elif "device_data" in config:
        fail("target device_data requires a bluetooth, audio-profile or FM radio group")
    else:
        config["device_data"] = {"groups": {}}
    config["rootfs"] = {
        "base_packages": target_rootfs_packages,
        "packages": selected_profile["rootfs"]["packages"],
        "exclude_packages": [],
    }
    config["profile"] = profile
    platform_bootstrap = platform["bootstrap"]
    platform_runtime = platform["runtime"]
    profile_bootstrap = selected_profile["bootstrap"]
    bootstrap_kind = profile_bootstrap["kind"]
    profile_layout = selected_profile["layout"]
    stage0_layout = profile_layout if isinstance(profile_layout, dict) else None
    if bootstrap_kind == "uboot-stage0":
        if stage0_layout is None:
            fail(f"profile {profile} resident U-Boot stage has no layout")
        if stage0_layout["resident_start"] != platform_bootstrap["load_address"]:
            fail(f"profile {profile} resident stage must start at the platform load address")
    config["bootstrap"] = {
        "kind": bootstrap_kind,
        "source": (
            profile_bootstrap["source"] if bootstrap_kind == "uboot-stage0" else "bootstrap"
        ),
        "image": (
            profile_bootstrap["image"] if bootstrap_kind == "uboot-stage0" else bootstrap["image"]
        ),
        "map": (
            profile_bootstrap["map"] if bootstrap_kind == "uboot-stage0" else bootstrap["map"]
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
    for key in ("uboot", "fit", "layout", "storage", "image"):
        config[key] = selected_profile[key]
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
        "runnable": selected_profile["runtime"]["runnable"],
    }
    return config
