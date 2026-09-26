# SPDX-License-Identifier: GPL-2.0-only
"""Load platform integration and tool recipes."""

from __future__ import annotations

from typing import Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.identity import IdentityError, validate_platform_identity
from fplinux_cli.manifests.values import (
    TARGET_NAME,
    exact_table,
    integer_value,
    kconfig_line_array,
    nonempty_string,
    package_array,
    path_array,
    path_steps,
    relative_value,
    string_array,
    validate_usb,
)


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
    path = common.ROOT / "platforms" / platform / "platform.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown platform: {platform}")
    raw = common.load_toml(path)
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
            "uboot",
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
            "defconfig",
            "arch",
            "cross_compile",
            "analysis_cross_compile",
            "config_script",
            "image_output",
            "dtb_output_directory",
            "dts_directory",
            "platform_identity_header",
            "targets",
            "patches",
            "copies",
            "appends",
        },
        "platform linux",
    )
    for key in ("source_lock", "arch", "cross_compile", "analysis_cross_compile"):
        nonempty_string(linux.get(key), f"platform linux {key}")
    for key in (
        "defconfig",
        "config_script",
        "image_output",
        "dtb_output_directory",
        "dts_directory",
        "platform_identity_header",
    ):
        relative_value(linux.get(key), f"platform linux {key}")
    string_array(linux.get("targets"), "platform linux targets")
    path_array(linux.get("patches"), "platform linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "platform linux copies")
    path_steps(linux.get("appends"), "platform linux appends")

    uboot = exact_table(
        config.get("uboot"),
        {"source", "archive_prefix", "patches", "required_config", "copies"},
        "platform uboot",
    )
    for key in ("source", "archive_prefix"):
        relative_value(uboot[key], f"platform uboot {key}")
    path_array(uboot.get("patches"), "platform uboot patches", allow_empty=True)
    kconfig_line_array(uboot.get("required_config"), "platform uboot required_config")
    path_steps(uboot.get("copies"), "platform uboot copies")

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
            "patches",
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
    path_array(bootstrap.get("patches"), "platform bootstrap patches", allow_empty=True)
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
