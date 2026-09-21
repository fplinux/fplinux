# SPDX-License-Identifier: GPL-2.0-only
"""Validate manifest values at their input boundary."""

from __future__ import annotations

import re
from pathlib import Path, PurePosixPath
from typing import Any

from fplinux_cli.common import fail, relative_name

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


def sha256_value(value: object, name: str) -> str:
    """Require one lowercase SHA-256 digest."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def firmware_array(value: object, name: str) -> list[dict[str, Any]]:
    """Validate explicitly named inputs and their firmware lookup destinations."""
    if not isinstance(value, list) or not value:
        fail(f"{name} must be a non-empty array")
    result: list[dict[str, Any]] = []
    sources: set[str] = set()
    destinations: set[str] = set()
    for index, raw in enumerate(value):
        item_name = f"{name}[{index}]"
        if not isinstance(raw, dict) or set(raw) not in (
            {"source", "destination", "size"},
            {"source", "destination", "size", "sha256"},
        ):
            fail(f"{item_name} must contain exactly source, destination, size and optional sha256")
        source = basename_value(raw.get("source"), f"{item_name} source")
        if source in sources:
            fail(f"{name} must not contain duplicate sources: {source}")
        sources.add(source)
        destination = relative_value(raw.get("destination"), f"{item_name} destination")
        if not PurePosixPath(destination).name:
            fail(f"{item_name} destination must name a file in the firmware namespace")
        if destination in destinations:
            fail(f"{name} must not contain duplicate destinations: {destination}")
        destinations.add(destination)
        normalized: dict[str, Any] = {
            "source": source,
            "destination": destination,
            "size": integer_value(raw.get("size"), f"{item_name} size", bounds=(1, 0xFFFFFFFF)),
        }
        if "sha256" in raw:
            normalized["sha256"] = sha256_value(raw.get("sha256"), f"{item_name} sha256")
        result.append(normalized)
    return result
