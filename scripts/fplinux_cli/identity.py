# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- identity failures use exact shared protocol diagnostics.
"""Canonical target and platform identity contracts."""

from __future__ import annotations

import re
from typing import Any

_HUMAN_TEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9 .&()+/'-]*\Z")
_HARDWARE_CODE = re.compile(r"[A-Z0-9][A-Z0-9._-]*\Z")
_COMPATIBLE = re.compile(r"[a-z0-9][a-z0-9.-]*,[a-z0-9][a-z0-9+._-]*\Z")
_PLATFORM_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*\Z")

RUNTIME_IDENTITY_PATH = "runner/identity.py"


class IdentityError(ValueError):
    """Reject an ambiguous or non-canonical identity declaration."""


def _exact_table(value: object, keys: set[str], name: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        expected = ", ".join(sorted(keys))
        raise IdentityError(f"{name} must contain exactly: {expected}")
    return value


def _human_text(value: object, name: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or value != value.strip()
        or "  " in value
        or _HUMAN_TEXT.fullmatch(value) is None
    ):
        raise IdentityError(f"{name} must be canonical printable ASCII text")
    return value


def _hardware_code(value: object, name: str) -> str:
    if not isinstance(value, str) or _HARDWARE_CODE.fullmatch(value) is None:
        raise IdentityError(f"{name} must be an uppercase hardware identifier")
    return value


def _hardware_codes(value: object, name: str) -> list[str]:
    if not isinstance(value, list):
        raise IdentityError(f"{name} must be an array")
    result = [_hardware_code(item, name) for item in value]
    folded = [item.casefold() for item in result]
    if len(folded) != len(set(folded)):
        raise IdentityError(f"{name} must not contain duplicates")
    return result


def _compatible(value: object, name: str) -> str:
    if not isinstance(value, str) or _COMPATIBLE.fullmatch(value) is None:
        raise IdentityError(f"{name} must be a lowercase vendor,device compatible")
    return value


def target_display_name(identity: dict[str, Any]) -> str:
    """Format the public device name from one normalized target identity."""
    result = f"{identity['brand']} {identity['product']}"
    codes = identity["hardware_codes"]
    if codes:
        result += f" ({', '.join(codes)})"
    return result


def platform_display_name(identity: dict[str, Any]) -> str:
    """Format the public SoC platform name from normalized identity data."""
    return f"{identity['vendor']} {identity['soc']}"


def validate_target_identity(value: object, name: str = "target identity") -> dict[str, Any]:
    """Validate and normalize one target-owned hardware identity."""
    table = _exact_table(
        value,
        {"brand", "product", "hardware_codes", "compatible"},
        name,
    )
    result: dict[str, Any] = {
        "brand": _human_text(table.get("brand"), f"{name} brand"),
        "product": _human_text(table.get("product"), f"{name} product"),
        "hardware_codes": _hardware_codes(table.get("hardware_codes"), f"{name} hardware_codes"),
        "compatible": _compatible(table.get("compatible"), f"{name} compatible"),
    }
    result["display_name"] = target_display_name(result)
    return result


def validate_platform_identity(value: object, name: str = "platform identity") -> dict[str, Any]:
    """Validate and normalize one platform-owned SoC identity."""
    table = _exact_table(value, {"vendor", "soc", "aliases", "compatible"}, name)
    soc = _hardware_code(table.get("soc"), f"{name} soc")
    aliases = _hardware_codes(table.get("aliases"), f"{name} aliases")
    if soc.casefold() in {alias.casefold() for alias in aliases}:
        raise IdentityError(f"{name} aliases must not repeat the SoC name")
    result: dict[str, Any] = {
        "vendor": _human_text(table.get("vendor"), f"{name} vendor"),
        "soc": soc,
        "aliases": aliases,
        "compatible": _compatible(table.get("compatible"), f"{name} compatible"),
    }
    result["display_name"] = platform_display_name(result)
    return result


def validate_platform_name(value: object, name: str = "runtime platform name") -> str:
    """Validate the lowercase slug naming one platform implementation."""
    if not isinstance(value, str) or _PLATFORM_NAME.fullmatch(value) is None:
        raise IdentityError(f"{name} must be a lowercase platform identifier")
    return value


def validate_runtime_identity(value: object) -> dict[str, dict[str, Any]]:
    """Validate the exact structured identity published in a runtime bundle."""
    identity = _exact_table(value, {"platform", "target"}, "runtime identity")
    target = _exact_table(
        identity.get("target"),
        {"brand", "product", "hardware_codes", "compatible", "display_name"},
        "runtime target identity",
    )
    normalized_target = validate_target_identity(
        {key: target[key] for key in ("brand", "product", "hardware_codes", "compatible")},
        "runtime target identity",
    )
    if target.get("display_name") != normalized_target["display_name"]:
        raise IdentityError("runtime target display_name must be derived from its identity fields")

    platform = _exact_table(
        identity.get("platform"),
        {"name", "vendor", "soc", "aliases", "compatible", "display_name"},
        "runtime platform identity",
    )
    platform_name = validate_platform_name(platform.get("name"))
    normalized_platform = validate_platform_identity(
        {key: platform[key] for key in ("vendor", "soc", "aliases", "compatible")},
        "runtime platform identity",
    )
    if platform.get("display_name") != normalized_platform["display_name"]:
        raise IdentityError(
            "runtime platform display_name must be derived from its identity fields"
        )
    if normalized_target["compatible"] == normalized_platform["compatible"]:
        raise IdentityError("runtime target and platform compatibles must differ")
    return {
        "target": normalized_target,
        "platform": {"name": platform_name, **normalized_platform},
    }
