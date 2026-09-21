# SPDX-License-Identifier: GPL-2.0-only
"""Compose the platform and target kernel configuration."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.paths import target_directory

if TYPE_CHECKING:
    from pathlib import Path


def kernel_config_paths(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> tuple[Path, Path]:
    """Return the shared Linux base and the selected board's hardware fragment."""
    return (
        common.ROOT / platform["linux"]["defconfig"],
        target_directory(target) / target_config["linux"]["config_fragment"],
    )


def kconfig_values(contents: str) -> dict[str, str]:
    """Read explicit assignments and disabled symbols from a Kconfig input."""
    values: dict[str, str] = {}
    for line in contents.splitlines():
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
        elif line.startswith("CONFIG_") and "=" in line:
            symbol, value = line.split("=", 1)
            values[symbol] = value
        elif line and not line.startswith("#"):
            fail(f"invalid Kconfig input line: {line}")
    return values


def compose_kernel_config(base: Path, fragment: Path) -> bytes:
    """Apply board assignments over the shared base for both Kbuild consumers."""
    values: dict[str, str] = {}
    for path in (base, fragment):
        if path.is_symlink() or not path.is_file():
            fail(f"kernel configuration input is missing or invalid: {path}")
        values.update(kconfig_values(path.read_text()))
    return "".join(
        f"# {symbol} is not set\n" if value == "n" else f"{symbol}={value}\n"
        for symbol, value in values.items()
    ).encode()
