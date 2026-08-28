# SPDX-License-Identifier: GPL-2.0-only
"""Render transient files from one normalized microSD profile layout."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Mapping


def boot_layout_header(layout: Mapping[str, int]) -> bytes:
    """Return the C constants shared by the resident stage and U-Boot."""
    license_tag = "SPDX-License-" + "Identifier"
    fields = {
        "RAM_BASE_PHYS": layout["ram_base"],
        "RAM_REQUIRED_BYTES": layout["ram_size"],
        "RAM_LIMIT_PHYS": layout["ram_base"] + layout["ram_size"],
        "TIMER_HZ": layout["timer_hz"],
        "ZIMAGE_PHYS": layout["kernel_load"],
        "ZIMAGE_ENTRY_PHYS": layout["kernel_entry"],
        "ZIMAGE_LIMIT_BYTES": layout["kernel_size"],
        "DTB_PHYS": layout["fdt_load"],
        "DTB_LIMIT_BYTES": layout["fdt_size"],
        "FRAMEBUFFER_PHYS": layout["framebuffer"],
        "FRAMEBUFFER_BYTES": layout["framebuffer_size"],
    }
    if "uboot_load" in layout:
        fields.update(
            {
                "RESIDENT_START_PHYS": layout["resident_start"],
                "RESIDENT_LIMIT_PHYS": layout["resident_limit"],
                "UBOOT_LOAD_PHYS": layout["uboot_load"],
                "UBOOT_LIMIT_BYTES": layout["uboot_size"],
                "UBOOT_STACK_PHYS": layout["uboot_stack"],
                "FIT_PHYS": layout["fit_load"],
                "FIT_LIMIT_BYTES": layout["fit_size"],
            }
        )
    defines = "".join(
        f"#define FPLINUX_BOOT_LAYOUT_{name} 0x{value:08x}U\n" for name, value in fields.items()
    )
    return (
        f"/* {license_tag}: GPL-2.0-only */\n"
        "/* Generated from the selected build profile. */\n"
        "#ifndef FPLINUX_BOOT_LAYOUT_H\n"
        "#define FPLINUX_BOOT_LAYOUT_H\n\n"
        f"{defines}\n"
        "#endif\n"
    ).encode("ascii")


def bootstrap_memory_ld(bootstrap: Mapping[str, Any], layout: Mapping[str, int]) -> bytes:
    """Return the linker ranges selected for one resident bootstrap."""
    ram_base = layout["ram_base"]
    origin = bootstrap["load_address"]
    limit = bootstrap["payload_limit"]
    if (
        type(ram_base) is not int
        or type(origin) is not int
        or type(limit) is not int
        or not 0 <= ram_base < origin < limit
    ):
        message = "bootstrap linker memory range is invalid"
        raise ValueError(message)
    return f"""EXTRA_START = 0x{ram_base:08x};
EXTRA_SIZE = 0x{origin - ram_base:08x};
IMAGE_START = 0x{origin:08x};
IMAGE_SIZE = 0x{limit - origin:08x};
""".encode("ascii")


def uboot_defconfig(base: bytes, layout: Mapping[str, int]) -> bytes:
    """Add the profile-owned U-Boot placement to its stable base config."""
    text = base.decode("ascii")
    owned = (
        "CONFIG_TEXT_BASE=",
        "CONFIG_CUSTOM_SYS_INIT_SP_ADDR=",
        "CONFIG_SYS_LOAD_ADDR=",
        "CONFIG_SYS_FDT_PAD=",
    )
    if any(line.startswith(owned) for line in text.splitlines()):
        message = "U-Boot base defconfig contains profile-owned layout values"
        raise ValueError(message)
    return (
        text.rstrip()
        + "\n"
        + f"CONFIG_TEXT_BASE=0x{layout['uboot_load']:08x}\n"
        + f"CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x{layout['uboot_stack']:08x}\n"
        + f"CONFIG_SYS_LOAD_ADDR=0x{layout['fit_load']:08x}\n"
        + f"CONFIG_SYS_FDT_PAD=0x{layout['fdt_pad']:08x}\n"
    ).encode("ascii")


def external_root_dtsi(root: Mapping[str, Any]) -> bytes:
    """Return the selected profile's exact external-root bootargs property."""
    bootargs = (
        "console=tty0 loglevel=8 ignore_loglevel "
        f"root=PARTUUID={root['partuuid']} "
        f"rootfstype={root['filesystem']} rootwait={root['wait_seconds']} rw "
        "init=/sbin/init panic=-1 vt.global_cursor_default=1 "
        "random.trust_bootloader=on"
    )
    return f'bootargs = "{bootargs}";\n'.encode("ascii")


def uboot_layout_dtsi(layout: Mapping[str, int]) -> bytes:
    """Return the U-Boot control tree's RAM node from the shared layout."""
    return f"""#define FPLINUX_UBOOT_TIMER_HZ {layout["timer_hz"]}
memory@{layout["ram_base"]:x} {{
\tdevice_type = "memory";
\treg = <0x{layout["ram_base"]:08x} 0x{layout["ram_size"]:08x}>;
}};
""".encode("ascii")


def genimage_config(fit: Mapping[str, Any], storage: Mapping[str, Any]) -> bytes:
    """Return one deterministic MBR/FAT/ext4 genimage configuration."""
    license_tag = "SPDX-License-" + "Identifier"
    signature = storage["disk_signature"]
    image = storage["filename"]
    return f"""# {license_tag}: GPL-2.0-only
image {image} {{
\thdimage {{
\t\talign = 1M
\t\tpartition-table-type = "mbr"
\t\tdisk-signature = 0x{signature:08x}
\t}}

\tpartition boot {{
\t\tpartition-type = 0x0c
\t\timage = "FPLINUX.vfat"
\t\toffset = {storage["boot_offset"]}
\t\tsize = {storage["boot_size"]}
\t}}

\tpartition root {{
\t\tpartition-type = 0x83
\t\timage = "{storage["root_filename"]}"
\t\toffset = {storage["root_offset"]}
\t\tsize = {storage["root_size"]}
\t\tfill = true
\t}}
}}

image FPLINUX.vfat {{
\ttemporary = true
\tsize = {storage["boot_size"]}

\tvfat {{
\t\tlabel = "{storage["boot_label"]}"
\t\textraargs = "-F 32 --invariant -i {signature:08x}"

\t\tfile {fit["filename"]} {{
\t\t\timage = "{fit["filename"]}"
\t\t}}
\t}}
}}
""".encode("ascii")
