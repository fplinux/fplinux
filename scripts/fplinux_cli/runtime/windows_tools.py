# SPDX-License-Identifier: GPL-2.0-only
"""Native tool paths shared by Windows delivery and runtime admission."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping

WINDOWS_TOOLS = {
    "loader": "tools/spd_dump.exe",
    "bridge": "tools/libc_server.exe",
    "ssh": "tools/openssh/ssh.exe",
    "keygen": "tools/openssh/ssh-keygen.exe",
    "keyscan": "tools/openssh/ssh-keyscan.exe",
    "driver": "tools/driver/wdi-simple.exe",
}


def windows_tool_paths(runtime_tools: Mapping[str, str]) -> dict[str, str]:
    """Add shared host services to the selected platform's native tools."""
    return {
        role: path
        for role, path in WINDOWS_TOOLS.items()
        if role not in {"loader", "bridge"} or role in runtime_tools
    }
