# SPDX-License-Identifier: GPL-2.0-only
"""Load target-owned panel commands and shared electrical settings."""

from __future__ import annotations

import tomllib
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.values import (
    TARGET_NAME,
    exact_table,
    integer_value,
    nonempty_string,
    relative_value,
    string_array,
)

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path


def _commands(value: object, name: str) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        fail(f"{name} must be an array")
    result: list[dict[str, Any]] = []
    for index, raw in enumerate(value):
        label = f"{name}[{index}]"
        if not isinstance(raw, dict) or not raw or set(raw) - {"command", "data", "delay_ms"}:
            fail(f"{label} must contain command, data and/or delay_ms")
        if "command" not in raw and (set(raw) != {"delay_ms"} or not raw["delay_ms"]):
            fail(f"{label} without a command must contain a nonzero delay_ms")
        command = integer_value(raw.get("command", 0), f"{label} command", bounds=(0, 255))
        delay_ms = integer_value(raw.get("delay_ms", 0), f"{label} delay_ms", bounds=(0, 8191))
        data = raw.get("data", [])
        if not isinstance(data, list) or len(data) > 16:
            fail(f"{label} data must contain at most 16 bytes")
        for byte_index, byte in enumerate(data):
            integer_value(byte, f"{label} data[{byte_index}]", bounds=(0, 255))
        if command == 0 and not data and not delay_ms:
            fail(f"{label} must send a command or wait")
        result.append({"command": command, "data": data, "delay_ms": delay_ms})
    return result


def _pins(value: object, name: str) -> list[list[int]]:
    if not isinstance(value, list) or not value:
        fail(f"{name} must contain pin offset, mux and configuration triples")
    result: list[list[int]] = []
    offsets: set[int] = set()
    for index, pin in enumerate(value):
        if not isinstance(pin, list) or len(pin) != 3:
            fail(f"{name}[{index}] must contain offset, mux and configuration")
        offset = integer_value(pin[0], f"{name}[{index}] offset", bounds=(0, 0x3FF), alignment=4)
        if offset in offsets:
            fail(f"{name} must not repeat a pin offset")
        offsets.add(offset)
        for field, item in zip(("mux", "configuration"), pin[1:], strict=True):
            integer_value(item, f"{name}[{index}] {field}", bounds=(0, 0xFFFFFFFF))
        result.append(pin)
    return result


def load_display(
    target: str,
    raw: object,
    *,
    root: Path | None = None,
    captured: Mapping[str, bytes] | None = None,
) -> dict[str, Any]:
    """Normalize a panel profile, or zero geometry for a headless target."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    if raw is None:
        raw = {"boot_font": "6x8"}
    display = exact_table(
        raw,
        {"boot_font", *({"panel"} if isinstance(raw, dict) and "panel" in raw else set())},
        "target display",
    )
    boot_font = nonempty_string(display["boot_font"], "target display boot_font")
    if boot_font not in {"6x8", "7x14"}:
        fail("target display boot_font must be 6x8 or 7x14")
    if "panel" not in display:
        return {"width": 0, "height": 0, "boot_font": boot_font, "panel": None, "source": None}
    source = relative_value(display["panel"], "target display panel")
    path = (root if root is not None else common.ROOT) / "targets" / target / source
    if captured is None:
        if path.is_symlink() or not path.is_file():
            fail(f"target display panel is missing or invalid: {path}")
        panel_raw = load_toml(path)
    else:
        relative = f"targets/{target}/{source}"
        contents = captured.get(relative)
        if contents is None:
            fail(f"target display panel is missing from captured sources: {relative}")
        try:
            panel_raw = tomllib.loads(contents.decode("utf-8"))
        except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
            fail(f"invalid TOML in {relative}: {error}")
    panel = exact_table(
        panel_raw,
        {
            "width",
            "height",
            "id",
            "id_mask",
            "memory_access",
            "transport",
            "license",
            "copyright",
            "commands",
            "bootstrap",
            "linux",
            "pins",
            *({"dbi"} if "dbi" in panel_raw else set()),
        },
        "target display panel",
    )
    for field in ("width", "height"):
        integer_value(panel[field], f"target display panel {field}", bounds=(1, 65535))
    for field in ("id", "id_mask"):
        integer_value(panel[field], f"target display panel {field}", bounds=(0, 0xFFFFFFFF))
    integer_value(panel["memory_access"], "target display panel memory_access", bounds=(0, 255))
    if panel["transport"] not in {"spi", "dbi"}:
        fail("target display panel transport must be spi or dbi")
    if panel["license"] not in {"GPL-2.0-only", "GPL-2.0-only AND Unlicense"}:
        fail("target display panel license is unsupported")
    string_array(panel["copyright"], "target display panel copyright", allow_empty=True)
    panel["commands"] = _commands(panel["commands"], "target display panel commands")
    raw_bootstrap = panel["bootstrap"]
    bootstrap = exact_table(
        raw_bootstrap,
        {
            "spi_hz",
            "prepare_pins",
            "commands_after",
            *(
                {"reset_ms"}
                if isinstance(raw_bootstrap, dict) and "reset_ms" in raw_bootstrap
                else set()
            ),
        },
        "target display panel bootstrap",
    )
    integer_value(
        bootstrap["spi_hz"], "target display panel bootstrap spi_hz", bounds=(0, 0xFFFFFFFF)
    )
    if type(bootstrap["prepare_pins"]) is not bool:
        fail("target display panel bootstrap prepare_pins must be a boolean")
    if "reset_ms" in bootstrap:
        integer_value(
            bootstrap["reset_ms"], "target display panel bootstrap reset_ms", bounds=(0, 65535)
        )
    linux = exact_table(
        panel["linux"],
        {"commands_after", "cold_finish", "wake_finish"},
        "target display panel linux",
    )
    for field in ("commands_after", "cold_finish"):
        linux[field] = _commands(linux[field], f"target display panel linux {field}")
    linux["wake_finish"] = (
        linux["cold_finish"]
        if linux["wake_finish"] == "cold_finish"
        else _commands(linux["wake_finish"], "target display panel linux wake_finish")
    )
    bootstrap["commands_after"] = (
        linux["cold_finish"]
        if bootstrap["commands_after"] == "cold_finish"
        else _commands(
            bootstrap["commands_after"], "target display panel bootstrap commands_after"
        )
    )
    if panel["transport"] == "dbi":
        dbi = exact_table(panel.get("dbi"), {"timing_ns"}, "target display panel dbi")
        timing = dbi["timing_ns"]
        if not isinstance(timing, list) or len(timing) != 6:
            fail("target display panel dbi timing_ns must contain six values")
        for index, value in enumerate(timing):
            integer_value(value, f"target display panel dbi timing_ns[{index}]", bounds=(0, 255))
    elif "dbi" in panel:
        fail("target display panel spi transport must not contain dbi timing")
    raw_pins = panel["pins"]
    pins = exact_table(
        raw_pins,
        {
            "control_base",
            "control",
            *(
                {"data_base", "data"}
                if isinstance(raw_pins, dict) and "data" in raw_pins
                else set()
            ),
        },
        "target display panel pins",
    )
    for group in ("control", "data"):
        if group in pins:
            integer_value(
                pins[f"{group}_base"],
                f"target display panel pins {group}_base",
                bounds=(0, 0x3FF),
                alignment=4,
            )
            pins[group] = _pins(pins[group], f"target display panel pins {group}")
    return {
        "width": panel["width"],
        "height": panel["height"],
        "boot_font": boot_font,
        "panel": panel,
        "source": source,
    }
