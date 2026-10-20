# SPDX-License-Identifier: GPL-2.0-only
"""Combine one board with its platform and boot profile."""

from __future__ import annotations

import re
from itertools import pairwise
from pathlib import PurePosixPath
from typing import Any

from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.assets import asset_bundle_paths, load_asset_sources
from fplinux_cli.manifests.audio import (
    AUDIO_POLICY_FIELDS,
    AudioPolicyError,
    audio_policy_from_profile,
)
from fplinux_cli.manifests.display import load_display
from fplinux_cli.manifests.identity import (
    IdentityError,
    validate_build_type,
    validate_record_prefix,
    validate_target_identity,
)
from fplinux_cli.manifests.linux import linux_storage_policy, normalize_linux_integration
from fplinux_cli.manifests.paths import (
    declared_boot_profiles,
    normalize_profile,
    platform_asset_lock_path,
    target_directory,
)
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
    relative_value,
)


def _sc_boot_layout(value: object, memory: dict[str, Any]) -> dict[str, int]:
    """Validate ordered RAM payload arenas before their actual artifact sizes are known."""
    name = "target bootstrap layout"
    fields = {
        "entry_load",
        "entry_limit",
        "kernel_load",
        "kernel_stage",
        "kernel_stage_limit",
        "fdt_load",
        "initramfs_load",
        "stack",
    }
    layout = exact_table(value, fields, name)
    normalized = {
        field: integer_value(layout[field], f"{name} {field}", bounds=(0, 0xFFFFFFFF), alignment=4)
        for field in fields
    }
    if not (
        memory["base"]
        <= normalized["entry_load"]
        < normalized["entry_limit"]
        <= normalized["kernel_load"]
        < normalized["kernel_stage"]
        < normalized["kernel_stage_limit"]
        <= normalized["fdt_load"]
        < normalized["initramfs_load"]
        < normalized["stack"]
        < memory["base"] + memory["size"]
    ):
        fail(f"{name} entry, payload and stack ranges must be ordered inside Linux memory")
    return normalized


def _target_bootstrap(value: object, backend: str, memory: dict[str, Any]) -> dict[str, Any]:
    """Validate board-owned inputs for the selected platform bootstrap."""
    fields = {"image", "map", "record_prefix"}
    if backend == "sc9820e":
        fields.add("layout")
    else:
        fields.add("dtb_destination")
        if isinstance(value, dict) and "lcd_config" in value:
            fields.add("lcd_config")
    bootstrap = exact_table(value, fields, "target bootstrap")
    for key in ("image", "map"):
        relative_value(bootstrap[key], f"target bootstrap {key}")
    try:
        bootstrap["record_prefix"] = validate_record_prefix(bootstrap["record_prefix"])
    except IdentityError as error:
        fail(str(error))
    if backend == "sc9820e":
        bootstrap["layout"] = _sc_boot_layout(bootstrap["layout"], memory)
    else:
        relative_value(bootstrap["dtb_destination"], "target bootstrap dtb_destination")
        bootstrap["lcd_config"] = (
            relative_value(bootstrap["lcd_config"], "target bootstrap lcd_config")
            if "lcd_config" in bootstrap
            else None
        )
    return bootstrap


def _target_adapter(value: object, backend: str, display: dict[str, Any]) -> dict[str, Any]:
    """Keep board display-loader settings confined to the UMS9117 adapter."""
    fields = {"session_name", "boot_instructions"}
    display_keys = {"spi_mode", "backlight_channels", "backlight_level"}
    has_display = isinstance(value, dict) and not display_keys.isdisjoint(value)
    if backend == "ums9117":
        fields.add("exec_distance")
        if has_display:
            fields.update(display_keys)
    adapter = exact_table(value, fields, "target adapter")
    if backend == "ums9117":
        integer_value(adapter["exec_distance"], "target adapter exec_distance", bounds=(0, 0xFFFF))
        # Without these settings the UMS9117 RAM loader runs headless.
        if has_display:
            integer_value(adapter["spi_mode"], "target adapter spi_mode", bounds=(0, 3))
            panel = display["panel"]
            if panel is None:
                fail("target adapter display settings require a panel")
            adapter = {**adapter, "lcd_id": panel["id"]}
            nonempty_string(adapter["backlight_channels"], "target adapter backlight_channels")
            integer_value(
                adapter["backlight_level"], "target adapter backlight_level", bounds=(0, 0x3F)
            )
    for key in ("session_name", "boot_instructions"):
        nonempty_string(adapter[key], f"target adapter {key}")
    return adapter


def _sc_loader(target: str, value: object) -> dict[str, Any]:
    """Validate the board-owned RAM placement and source-built loader selection."""
    loader = exact_table(
        value,
        {
            "fdl1_load",
            "sml_load",
            "fdl2_load",
            "stack_guard_start",
            "stack_guard_end",
            "build",
        },
        "target loader",
    )
    for role in ("fdl1", "sml", "fdl2"):
        integer_value(
            loader[f"{role}_load"],
            f"target loader {role}_load",
            bounds=(0, 0xFFFFFFFF),
            alignment=4,
        )
    for field in ("stack_guard_start", "stack_guard_end"):
        integer_value(loader[field], f"target loader {field}", bounds=(0, 0xFFFFFFFF), alignment=4)
    if loader["stack_guard_start"] >= loader["stack_guard_end"]:
        fail("target loader stack guard must have increasing bounds")
    build = exact_table(
        loader["build"],
        {"source", "fdl1_source", "fdl2_source", "sml_source"},
        "target loader build",
    )
    source = relative_value(build["source"], "target loader build source")
    source_path = target_directory(target) / source
    if source_path.is_symlink() or not source_path.is_dir():
        fail("target loader build source must name a regular source directory")
    for role in ("fdl1", "fdl2", "sml"):
        basename_value(build[f"{role}_source"], f"target loader build {role}_source")
    return loader


def _combined_bootstrap(
    bootstrap: dict[str, Any], platform: dict[str, Any], selected_profile: dict[str, Any]
) -> dict[str, Any]:
    """Derive one bootstrap and initramfs-delivery contract for downstream consumers."""
    backend = platform["backend"]
    profile_bootstrap = selected_profile["bootstrap"]
    kind = profile_bootstrap["kind"]
    if kind == "uboot-stage0" and backend != "ums9117":
        fail("uboot-stage0 requires the ums9117 bootstrap backend")
    stage0_layout = selected_profile["layout"] if kind == "uboot-stage0" else None
    if kind == "uboot-stage0":
        if stage0_layout is None:
            fail("resident U-Boot stage has no layout")
        if stage0_layout["resident_start"] != platform["load_address"]:
            fail("resident stage must start at the platform load address")
    if selected_profile["linux"]["root"]["kind"] == "external":
        initramfs_delivery = "none"
    elif backend == "ums9117":
        initramfs_delivery = "embedded"
    else:
        initramfs_delivery = "external"
    result: dict[str, Any] = {
        "backend": backend,
        "kind": kind,
        "source": profile_bootstrap["source"] if kind == "uboot-stage0" else "bootstrap",
        "image": profile_bootstrap["image"] if kind == "uboot-stage0" else bootstrap["image"],
        "map": profile_bootstrap["map"] if kind == "uboot-stage0" else bootstrap["map"],
        "record_prefix": bootstrap["record_prefix"],
        "toolchain": platform["toolchain"],
        "initramfs_delivery": initramfs_delivery,
    }
    if backend == "sc9820e":
        layout = bootstrap["layout"]
        result.update(
            layout=layout,
            load_address=layout["entry_load"],
            payload_limit=layout["entry_limit"],
        )
    else:
        result.update(
            lcd_config=bootstrap["lcd_config"],
            kernel_destination=platform["kernel_destination"],
            dtb_destination=bootstrap["dtb_destination"],
            load_address=(
                stage0_layout["resident_start"]
                if stage0_layout is not None
                else platform["load_address"]
            ),
            payload_limit=(
                stage0_layout["resident_limit"]
                if stage0_layout is not None
                else platform["payload_limit"]
            ),
            layout=stage0_layout if stage0_layout is not None else platform["layout"],
            lto=platform["lto"],
        )
    return result


def load_target(
    target: str, profile: str | None = None, *, build_type: str = "release"
) -> dict[str, Any]:
    """Combine shared boot policy with one board and platform configuration."""
    profile = normalize_profile(profile)
    try:
        build_type = validate_build_type(build_type)
    except IdentityError as error:
        fail(str(error))
    path = target_directory(target) / "target.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown target: {target}")
    raw = load_toml(path)
    config = exact_table(
        raw,
        {
            "identity",
            *({"microsd"} if "microsd" in raw else set()),
            *({"device_data"} if "device_data" in raw else set()),
            *({"bluetooth"} if "bluetooth" in raw else set()),
            *({"audio_profile"} if "audio_profile" in raw else set()),
            *({"fm_radio"} if "fm_radio" in raw else set()),
            *({"wcn_firmware"} if "wcn_firmware" in raw else set()),
            *({"loader"} if "loader" in raw else set()),
            *({"nand"} if "nand" in raw else set()),
            *({"display_brightness"} if "display_brightness" in raw else set()),
            *({"display"} if "display" in raw else set()),
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
    config["display"] = load_display(target, config.get("display"))
    if "display_brightness" in config:
        display_brightness = exact_table(
            config["display_brightness"], {"backlight", "levels"}, "target display_brightness"
        )
        backlight = nonempty_string(
            display_brightness["backlight"], "target display_brightness backlight"
        )
        if backlight in {".", ".."} or VALUE_NAME.fullmatch(backlight) is None:
            fail("target display_brightness backlight must be a device name")
        levels = display_brightness["levels"]
        if not isinstance(levels, list) or len(levels) != 11:
            fail("target display_brightness levels must contain exactly 11 raw values")
        for index, value in enumerate(levels):
            integer_value(value, f"target display_brightness levels[{index}]", bounds=(0, 65535))
        if levels[0] != 0 or any(a >= b for a, b in pairwise(levels)):
            fail("target display_brightness levels must start at 0 and strictly ascend")
    if "nand" in config:
        nand = config["nand"]
        if not isinstance(nand, dict):
            fail("target nand must be a table")
        chip_fields = {"raw_device", "id", "raw_page_bytes"}
        if set(nand) not in ({"raw_device"}, chip_fields, {*chip_fields, "ecc_format"}):
            fail(
                "target nand must contain raw_device and optionally both id and raw_page_bytes; "
                "ecc_format also requires the declared chip"
            )
        raw_device = nonempty_string(nand["raw_device"], "target nand raw_device")
        if re.fullmatch(r"/dev/[A-Za-z0-9][A-Za-z0-9._-]*", raw_device) is None:
            fail("target nand raw_device must name one device directly under /dev")
        if "id" in nand:
            integer_value(nand["id"], "target nand id", bounds=(0, 0xFFFF))
            integer_value(
                nand["raw_page_bytes"], "target nand raw_page_bytes", bounds=(1, 0xFFFFFFFF)
            )
        if "ecc_format" in nand and (
            nand["ecc_format"] != "ums9117-bch12"
            or config["platform"] != "ums9117"
            or nand["raw_page_bytes"] != 2176
        ):
            fail("target NAND ECC format must be ums9117-bch12 with 2176-byte UMS9117 pages")
    platform_name = nonempty_string(config.get("platform"), f"target {target} platform")
    if VALUE_NAME.fullmatch(platform_name) is None:
        fail(f"target {target} has invalid platform: {path}")
    platform = load_platform(platform_name)
    backend = platform["bootstrap"]["backend"]
    if backend == "sc9820e":
        if "nand" in config:
            fail("sc9820e targets must not declare NAND")
        config["loader"] = _sc_loader(target, config.get("loader"))
    elif "loader" in config:
        fail("target loader is only supported by the sc9820e bootstrap backend")
    bundle = exact_table(config.get("bundle"), {"packages"}, "target bundle")
    package_array(bundle.get("packages"), "target bundle packages")
    rootfs = exact_table(config.get("rootfs"), {"packages"}, "target rootfs")
    target_rootfs_packages = package_array(rootfs.get("packages"), "target rootfs packages")

    raw_linux = config.get("linux")
    linux = exact_table(
        raw_linux,
        {
            "dtb",
            "config_fragment",
            "memory",
            "debug_dtb",
            "patches",
            "overlay",
            "copies",
            "appends",
            "additional_forbidden_config",
            "additional_forbidden_dtb_markers",
            *(
                {"dt_config_checks"}
                if isinstance(raw_linux, dict) and "dt_config_checks" in raw_linux
                else set()
            ),
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
    if backend == "sc9820e" and not (
        memory["base"]
        <= config["loader"]["stack_guard_start"]
        < config["loader"]["stack_guard_end"]
        <= memory["base"] + memory["size"]
    ):
        fail("target loader stack guard must fit inside Linux memory")
    relative_value(linux.get("dtb"), "target linux dtb")
    relative_value(linux.get("debug_dtb"), "target linux debug_dtb")
    linux.update(
        normalize_linux_integration(
            linux,
            "target linux",
            root=path.parents[2],
            source_base=PurePosixPath("targets") / target,
        )
    )
    linux.update(
        linux_storage_policy(
            linux.pop("additional_forbidden_config"),
            linux.pop("additional_forbidden_dtb_markers"),
        )
    )

    bootstrap = _target_bootstrap(config.get("bootstrap"), backend, memory)
    adapter = _target_adapter(config.get("adapter"), backend, config["display"])
    if (profile or "default") not in declared_boot_profiles(config):
        fail(f"target {target} does not support profile {profile}")
    microsd: dict[str, Any] = {}
    if "microsd" in config:
        microsd_fields = {"linux_patches"}
        if backend == "ums9117":
            microsd_fields |= {"bootstrap", "uboot"}
        microsd = exact_table(config["microsd"], microsd_fields, "target microsd")
        path_array(microsd.get("linux_patches"), "target microsd linux_patches", allow_empty=True)
    selected_profile = load_profile(target, profile or "default", platform, microsd)
    if backend == "ums9117":
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
        "dt_config_checks": [*platform["linux"]["dt_config_checks"], *linux["dt_config_checks"]],
        "config_enable": profile_linux["config_enable"],
        "config_disable": profile_linux["config_disable"],
        "patches": [*linux["patches"], *profile_linux["patches"]],
        "root": profile_linux["root"],
    }
    device_data_groups: dict[str, list[dict[str, Any]]] = {}
    audio_fields = AUDIO_POLICY_FIELDS
    config["audio_policy"] = audio_policy_from_profile(None)
    if "bluetooth" in config:
        bluetooth = exact_table(config.pop("bluetooth"), {"firmware"}, "target bluetooth")
        device_data_groups["bluetooth"] = firmware_array(
            bluetooth["firmware"],
            "target bluetooth firmware",
            size_required=False,
        )
    if "audio_profile" in config:
        raw_audio_profile = config.pop("audio_profile")
        declared_audio_fields = (
            set(raw_audio_profile) if isinstance(raw_audio_profile, dict) else set()
        )
        audio_profile = exact_table(
            raw_audio_profile,
            {"firmware", *(audio_fields & declared_audio_fields)},
            "target audio-profile",
        )
        try:
            config["audio_policy"] = audio_policy_from_profile(audio_profile)
        except AudioPolicyError as error:
            fail(str(error))
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
    if "wcn_firmware" in config:
        wcn = exact_table(config.pop("wcn_firmware"), {"firmware"}, "target WCN firmware")
        device_data_groups["wcn-firmware"] = firmware_array(
            wcn["firmware"],
            "target WCN firmware",
        )
    if device_data_groups:
        device_data = config.get("device_data")
        if not isinstance(device_data, dict) or set(device_data) not in (
            {"parser"},
            {"originals_parser"},
        ):
            fail("target device_data must contain exactly one of parser or originals_parser")
        source_field = "originals_parser" if "originals_parser" in device_data else "parser"
        parser = basename_value(device_data[source_field], f"target device_data {source_field}")
        if not parser.endswith(".py"):
            fail(f"target device_data {source_field} must be a Python filename")
        config["device_data"] = {source_field: parser, "groups": device_data_groups}
    elif "device_data" in config:
        fail("target device_data requires a declared parser group")
    else:
        config["device_data"] = {"groups": device_data_groups}
    config["rootfs"] = {
        "base_packages": target_rootfs_packages,
        "packages": selected_profile["rootfs"]["packages"],
    }
    config["profile"] = profile
    config["build_type"] = build_type
    platform_runtime = platform["runtime"]
    config["bootstrap"] = _combined_bootstrap(bootstrap, platform["bootstrap"], selected_profile)
    for key in ("uboot", "fit", "layout", "storage", "image"):
        config[key] = selected_profile[key]
    transport = selected_profile["runtime"]["transport"]
    config["loader_assets"] = (
        load_asset_sources(platform_asset_lock_path(platform_name)) if backend == "ums9117" else []
    )
    runtime_adapter = {**platform_runtime["adapter"], **adapter}
    if backend == "sc9820e":
        runtime_adapter["loader"] = {
            key: value for key, value in config["loader"].items() if key != "build"
        }
    config["runtime"] = {
        "fdl1_load_address": (
            platform_runtime["fdl1_load_address"]
            if backend == "ums9117"
            else config["loader"]["fdl1_load"]
        ),
        "assets": (
            asset_bundle_paths(config["loader_assets"])
            if backend == "ums9117"
            else {role: f"assets/{role}.bin" for role in ("fdl1", "fdl2", "sml")}
        ),
        "adapter": runtime_adapter,
        "usb": platform_runtime["usb"],
        "transport": transport,
        "runnable": selected_profile["runtime"]["runnable"],
    }
    return config
