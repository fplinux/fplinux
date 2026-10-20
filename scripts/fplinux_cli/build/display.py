# SPDX-License-Identifier: GPL-2.0-only
"""Render build-only panel tables for the bootstrap and Linux consumers."""

from __future__ import annotations

from typing import Any

_SPDX_TAG = "SPDX" + "-License-Identifier"
_COPYRIGHT_TAG = "SPDX" + "-FileCopyrightText"


def _header(panel: dict[str, Any]) -> list[str]:
    lines = [f"/* {_SPDX_TAG}: {panel['license']} */"]
    lines.extend(f"/* {_COPYRIGHT_TAG}: {name} */" for name in panel["copyright"])
    return [*lines, "/* Generated from the target panel profile. */", ""]


def render_bootstrap_display(display: dict[str, Any]) -> bytes | None:
    """Render data included by the target's existing bootstrap hooks."""
    panel = display["panel"]
    if panel is None or panel["renderer"] != "ums9117":
        return None
    lines = _header(panel)
    bootstrap = panel["bootstrap"]
    lines.append(f"#define FPLINUX_LCD_SPI {int(panel['transport'] == 'spi')}")
    if "reset_ms" in bootstrap:
        lines.append(f"#define FPLINUX_LCD_INIT_RESET_MS {bootstrap['reset_ms']}")
    if bootstrap["prepare_pins"]:
        lines.extend(
            [
                "",
                '#include "panel-pads.h"',
                "",
                "static const struct ums9117_bootstrap_panel_pad fplinux_panel_pads[] = {",
            ]
        )
        for group in ("control", "data"):
            pins = panel["pins"]
            if group not in pins:
                continue
            for offset, mux, config in pins[group]:
                address = pins[f"{group}_base"] + offset
                lines.append(f"\t{{ 0x{address:03x}, 0x{mux:08x} }},")
                lines.append(f"\t{{ 0x{address + 0x400:03x}, 0x{config:08x} }},")
        lines.extend(
            [
                "};",
                "",
                "#define FPLINUX_LCD_PREPARE() \\",
                "\tums9117_bootstrap_prepare_panel_pads(fplinux_panel_pads, \\",
                "\t\tsizeof(fplinux_panel_pads) / sizeof(fplinux_panel_pads[0]))",
            ]
        )
    lines.extend(["", "static const uint8_t fplinux_panel_init[] = {"])
    for step in [*panel["commands"], *bootstrap["commands_after"]]:
        if step["command"] or step["data"]:
            payload = ", ".join(f"0x{byte:02x}" for byte in step["data"])
            line = f"\tLCM_CMD(0x{step['command']:02x}, {len(step['data'])}),"
            if payload:
                line += f" {payload},"
            lines.append(line)
        if step["delay_ms"]:
            lines.append(f"\tLCM_DELAY({step['delay_ms']}),")
    lines.extend(
        ["\tLCM_END,", "};", "", "static const lcd_config_t lcd_config_t117[] = {", "\t{"]
    )
    timing = panel["dbi"]["timing_ns"] if panel["transport"] == "dbi" else [0] * 6
    lines.extend(
        [
            f"\t\t0x{panel['id']:x}, 0x{panel['id_mask']:x},",
            f"\t\t{panel['width']}, {panel['height']}, 0x{panel['memory_access']:02x},",
            "\t\t{ " + ", ".join(map(str, timing)) + " },",
            f"\t\t{{ {bootstrap['spi_hz']} }},",
            "\t\tfplinux_panel_init,",
            "\t},",
            "};",
            "",
        ]
    )
    return "\n".join(lines).encode("ascii")


def _linux_commands(name: str, commands: list[dict[str, Any]]) -> list[str]:
    if not commands:
        return []
    lines = [f"static const struct ums9117_drm_command {name}[] = {{"]
    for step in commands:
        payload = ", ".join(f"0x{byte:02x}" for byte in step["data"])
        lines.append(
            f"\t{{ 0x{step['command']:02x}, {len(step['data'])}, "
            f"{step['delay_ms']}, {{ {payload} }} }},"
        )
    return [*lines, "};", ""]


def linux_panel_header_name(target: str) -> str:
    """Give every target its own generated include in a shared Linux source tree."""
    return f"fplinux-{target}-panel.h"


def linux_panel_dtsi_name(target: str) -> str:
    """Name the target's generated display-node property include."""
    return f"fplinux-{target}-panel.dtsi"


def linux_panel_addresses_header_name(target: str) -> str:
    """Name the shared pin-bank bases used by one target's DT resources."""
    return f"fplinux-{target}-panel-addresses.h"


def render_mipi_dbi_firmware(panel: dict[str, Any]) -> bytes:
    """Encode the Linux panel command file, including its one-byte delays."""
    contents = bytearray(b"MIPI DBI" + b"\0" * 7 + b"\1")
    for step in panel["commands"]:
        if step["command"] or step["data"]:
            contents.extend((step["command"], len(step["data"]), *step["data"]))
        if step["delay_ms"]:
            contents.extend((0, 1, step["delay_ms"]))
    return bytes(contents)


def render_linux_display_files(target: str, display: dict[str, Any]) -> dict[str, bytes]:
    """Render target panel commands and their Linux DT properties."""
    panel = display["panel"]
    if panel is None:
        return {}
    if panel["renderer"] == "mipi-dbi":
        timing = (
            f"// {_SPDX_TAG}: GPL-2.0-only\n"
            "/* Generated from the target panel profile. */\n"
            f"hactive = <{panel['width']}>;\n"
            f"vactive = <{panel['height']}>;\n"
        )
        return {
            f"firmware/{panel['firmware']}": render_mipi_dbi_firmware(panel),
            f"arch/arm/boot/dts/unisoc/{linux_panel_dtsi_name(target)}": timing.encode("ascii"),
        }
    lines = _header(panel)
    transport = (
        "UMS9117_DRM_TRANSPORT_SPI1_3WIRE"
        if panel["transport"] == "spi"
        else "UMS9117_DRM_TRANSPORT_LCM_DBI"
    )
    lines.extend(
        [
            f"#define FPLINUX_PANEL_WIDTH {panel['width']}",
            f"#define FPLINUX_PANEL_HEIGHT {panel['height']}",
            f"#define FPLINUX_PANEL_TRANSPORT {transport}",
            "",
        ]
    )
    linux = panel["linux"]
    lines += _linux_commands("fplinux_panel_init", [*panel["commands"], *linux["commands_after"]])
    lines += _linux_commands("fplinux_panel_cold_finish", linux["cold_finish"])
    lines += _linux_commands("fplinux_panel_wake_finish", linux["wake_finish"])
    properties = [
        f"// {_SPDX_TAG}: GPL-2.0-only",
        "/* Generated from the target panel profile. */",
    ]
    addresses = [
        f"/* {_SPDX_TAG}: GPL-2.0-only */",
        "/* Generated from the target panel pin bases. */",
    ]
    for group in ("control", "data"):
        if group not in panel["pins"]:
            continue
        mux = 0x402A0000 + panel["pins"][f"{group}_base"]
        addresses.extend(
            [
                f"#define FPLINUX_PANEL_{group.upper()}_MUX_PHYS 0x{mux:08x}",
                f"#define FPLINUX_PANEL_{group.upper()}_CONFIG_PHYS 0x{mux + 0x400:08x}",
            ]
        )
    if panel["transport"] == "dbi":
        values = " ".join(map(str, panel["dbi"]["timing_ns"]))
        properties.append(f"sprd,dbi-timing-ns = <{values}>;")
    for group in ("data", "control"):
        if group not in panel["pins"]:
            continue
        suffix = "-data" if group == "data" else ""
        pins = panel["pins"][group]
        offsets = " ".join(f"0x{pin[0]:x}" for pin in pins)
        for kind, index in (("pinconf", 2), ("pinmux", 1)):
            values = " ".join(f"0x{pin[index]:x}" for pin in pins)
            properties.extend(
                [
                    f"sprd,{kind}{suffix}-offsets = <{offsets}>;",
                    f"sprd,{kind}{suffix}-values = <{values}>;",
                ]
            )
    return {
        f"drivers/gpu/drm/ums9117/{linux_panel_header_name(target)}": "\n".join(lines).encode(
            "ascii"
        ),
        f"arch/arm/boot/dts/unisoc/{linux_panel_dtsi_name(target)}": (
            "\n".join(properties) + "\n"
        ).encode("ascii"),
        f"arch/arm/boot/dts/unisoc/{linux_panel_addresses_header_name(target)}": (
            "\n".join(addresses) + "\n"
        ).encode("ascii"),
    }
