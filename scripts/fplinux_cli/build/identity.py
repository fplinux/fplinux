# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- generated-contract failures use exact diagnostics.
"""Generate build-only consumers of canonical identity data."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from fplinux_cli.manifests.identity import (
    IdentityError,
    validate_platform_name,
    validate_record_prefix,
)
from fplinux_cli.templates import substitute_template

BOOTSTRAP_IDENTITY_HEADER = "generated/fplinux-bootstrap-identity.h"
BOOT_SCREEN_IDENTITY_BYTES = 32
_SPDX_TAG = "SPDX" + "-License-Identifier"


def runtime_identity(
    target_identity: dict[str, Any], platform_name: str, platform_identity: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    """Return the generated structured identity published in a runtime bundle."""
    platform_name = validate_platform_name(platform_name)
    return {
        "target": dict(target_identity),
        "platform": {"name": platform_name, **platform_identity},
    }


def validate_bootstrap_display_name(display_name: str) -> str:
    """Validate a display name for the bootstrap's fixed NUL-terminated screen field."""
    try:
        encoded_name = display_name.encode("ascii")
    except UnicodeEncodeError as error:
        raise IdentityError("bootstrap display name must be ASCII") from error
    if len(encoded_name) >= BOOT_SCREEN_IDENTITY_BYTES:
        raise IdentityError(
            f"bootstrap display name must fit in {BOOT_SCREEN_IDENTITY_BYTES - 1} bytes"
        )
    return display_name


def bootstrap_identity_header(target_identity: dict[str, Any], prefix: str) -> bytes:
    """Generate the only C representation of target/bootstrap identity."""
    display_name = validate_bootstrap_display_name(target_identity["display_name"])
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


def linux_identity_dtsi_name(target: str) -> str:
    """Keep each board's generated DT identity distinct in a shared source tree."""
    return f"fplinux-{target}-identity.dtsi"


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


def uboot_identity_dtsi_name(target: str) -> str:
    """Keep each board's generated U-Boot root identity distinct."""
    return f"fplinux-{target}-uboot-identity.dtsi"


def uboot_identity_dtsi(
    target_identity: dict[str, Any], platform_identity: dict[str, Any]
) -> bytes:
    """Generate the U-Boot control tree's current root identity."""
    return (
        f"// {_SPDX_TAG}: GPL-2.0-only\n"
        "/ {\n"
        f'\tmodel = "{target_identity["display_name"]} U-Boot control tree";\n'
        f'\tcompatible = "{target_identity["compatible"]}", '
        f'"{platform_identity["compatible"]}";\n'
        "};\n"
    ).encode("ascii")


def linux_audio_policy_dtsi_name(target: str) -> str:
    """Keep the target-owned audio policy distinct in a shared Linux tree."""
    return f"fplinux-{target}-audio-policy.dtsi"


def linux_audio_policy_dtsi(policy: dict[str, bool]) -> bytes:
    """Project the three existing audio topology choices into DT properties."""
    fields = (
        ("headfree_headphones_only", "fplinux,headfree-headphones-only"),
        ("speaker_from_headphones", "fplinux,headphone-fed-speaker"),
        ("speaker_vibration", "fplinux,speaker-vibration"),
    )
    properties = "".join(
        f"\t{property_name};\n" for field, property_name in fields if policy[field]
    )
    return (f"// {_SPDX_TAG}: GPL-2.0-only\n" + "&audio {\n" + properties + "};\n").encode("ascii")


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


def linux_machine_binding_path(target_identity: dict[str, Any], *, arch: str) -> str:
    """Return the generated binding path owned by the exact machine compatible."""
    arch = validate_platform_name(arch, "Linux architecture")
    directory = "arm" if arch == "arm64" else arch
    return f"Documentation/devicetree/bindings/{directory}/{target_identity['compatible']}.yaml"


def linux_machine_binding(
    target_identity: dict[str, Any], platform_identity: dict[str, Any], *, arch: str
) -> bytes:
    """Generate the exact root-node binding for one target/platform pair."""
    display_name = target_identity["display_name"]
    platform_name = platform_identity["display_name"]
    compatible = target_identity["compatible"]
    fallback = platform_identity["compatible"]
    binding = linux_machine_binding_path(target_identity, arch=arch).removeprefix(
        "Documentation/devicetree/bindings/"
    )
    template = Path(__file__).with_name("machine-binding.txt.in").read_text(encoding="ascii")
    return substitute_template(
        template,
        {
            "BINDING": binding,
            "DISPLAY_NAME": display_name,
            "PLATFORM_NAME": platform_name,
            "COMPATIBLE": compatible,
            "FALLBACK": fallback,
        },
    ).encode("ascii")
