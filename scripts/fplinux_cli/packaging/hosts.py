# SPDX-License-Identifier: GPL-2.0-only
"""Supported hosts for the delivered phone runtimes."""

from __future__ import annotations

from typing import Any


def runtime_hosts(config: dict[str, Any]) -> tuple[str, ...]:
    """Return hosts with a ready-to-run archive for the selected RAM configuration."""
    if config["linux"]["root"]["kind"] != "initramfs":
        return ()
    if config["platform"] in {"ums9117", "sc9820e"}:
        return ("linux", "windows")
    return ()
