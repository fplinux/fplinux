# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- generated-contract failures use exact diagnostics.
"""Generate build-only consumers of canonical identity data."""

from __future__ import annotations

import re
from typing import Any

from .identity import IdentityError, validate_platform_name

BOOTSTRAP_IDENTITY_HEADER = "generated/fplinux-bootstrap-identity.h"
LINUX_IDENTITY_DTSI = "arch/arm/boot/dts/unisoc/fplinux-target-identity.dtsi"
LINUX_PLATFORM_IDENTITY_HEADER = "arch/arm/mach-ums9117/fplinux-platform-identity.h"
BOOT_SCREEN_IDENTITY_BYTES = 32
_RECORD_PREFIX = re.compile(r"[A-Z0-9][A-Z0-9_]*\Z")
_SPDX_TAG = "SPDX" + "-License-Identifier"


def validate_record_prefix(value: object, name: str = "bootstrap record_prefix") -> str:
    """Validate the diagnostic prefix emitted by one bootstrap image."""
    if not isinstance(value, str) or _RECORD_PREFIX.fullmatch(value) is None:
        raise IdentityError(f"{name} must be an uppercase record identifier")
    return value


def runtime_identity(
    target_identity: dict[str, Any], platform_name: str, platform_identity: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    """Return the generated structured identity published in a runtime bundle."""
    platform_name = validate_platform_name(platform_name)
    return {
        "target": dict(target_identity),
        "platform": {"name": platform_name, **platform_identity},
    }


def bootstrap_identity_header(target_identity: dict[str, Any], prefix: str) -> bytes:
    """Generate the only C representation of target/bootstrap identity."""
    display_name = target_identity["display_name"]
    try:
        encoded_name = display_name.encode("ascii")
    except UnicodeEncodeError as error:
        raise IdentityError("bootstrap display name must be ASCII") from error
    if len(encoded_name) >= BOOT_SCREEN_IDENTITY_BYTES:
        raise IdentityError(
            f"bootstrap display name must fit in {BOOT_SCREEN_IDENTITY_BYTES - 1} bytes"
        )
    prefix = validate_record_prefix(prefix)
    return (
        f"/* {_SPDX_TAG}: GPL-2.0-only */\n"
        "/* Generated from the selected target identity. */\n"
        "#ifndef FPLINUX_BOOTSTRAP_IDENTITY_H\n"
        "#define FPLINUX_BOOTSTRAP_IDENTITY_H\n\n"
        f'#define FPLINUX_BOOTSTRAP_DISPLAY_NAME "{display_name}"\n'
        f'#define FPLINUX_BOOTSTRAP_RECORD_PREFIX "{prefix}"\n'
        "\n"
        "#endif\n"
    ).encode("ascii")


def linux_identity_dtsi(
    target_identity: dict[str, Any], platform_identity: dict[str, Any]
) -> bytes:
    """Generate root DT identity properties from the two normalized manifests."""
    return (
        f"// {_SPDX_TAG}: GPL-2.0-only\n"
        "/* Generated from the selected target and platform identities. */\n"
        "/ {\n"
        f'\tmodel = "{target_identity["display_name"]}";\n'
        f'\tcompatible = "{target_identity["compatible"]}", '
        f'"{platform_identity["compatible"]}";\n'
        "};\n"
    ).encode("ascii")


def linux_platform_identity_header(platform_identity: dict[str, Any]) -> bytes:
    """Generate the ARM machine descriptor's platform identity constants."""
    return (
        f"/* {_SPDX_TAG}: GPL-2.0-only */\n"
        "/* Generated from the selected platform identity. */\n"
        "#ifndef FPLINUX_PLATFORM_IDENTITY_H\n"
        "#define FPLINUX_PLATFORM_IDENTITY_H\n\n"
        f'#define FPLINUX_PLATFORM_DISPLAY_NAME "{platform_identity["display_name"]}"\n'
        f'#define FPLINUX_PLATFORM_COMPATIBLE "{platform_identity["compatible"]}"\n\n'
        "#endif\n"
    ).encode("ascii")


def linux_machine_binding_path(target_identity: dict[str, Any]) -> str:
    """Return the generated binding path owned by the exact machine compatible."""
    return f"Documentation/devicetree/bindings/arm/{target_identity['compatible']}.yaml"


def linux_machine_binding(
    target_identity: dict[str, Any], platform_identity: dict[str, Any]
) -> bytes:
    """Generate the exact root-node binding for one target/platform pair."""
    display_name = target_identity["display_name"]
    platform_name = platform_identity["display_name"]
    compatible = target_identity["compatible"]
    fallback = platform_identity["compatible"]
    return (
        f"# {_SPDX_TAG}: (GPL-2.0-only OR BSD-2-Clause)\n"
        "%YAML 1.2\n"
        "---\n"
        f"$id: http://devicetree.org/schemas/arm/{compatible}.yaml#\n"
        "$schema: http://devicetree.org/meta-schemas/core.yaml#\n\n"
        f"title: {display_name} on {platform_name}\n\n"
        "maintainers:\n"
        "  - FPLinux contributors <313246064+fplinux-dev@users.noreply.github.com>\n\n"
        "properties:\n"
        "  $nodename:\n"
        "    const: '/'\n"
        "  model:\n"
        f"    const: {display_name}\n"
        "  compatible:\n"
        "    items:\n"
        f"      - const: {compatible}\n"
        f"      - const: {fallback}\n\n"
        "required:\n"
        "  - model\n"
        "  - compatible\n\n"
        "additionalProperties: true\n\n"
        "...\n"
    ).encode("ascii")
