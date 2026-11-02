# SPDX-License-Identifier: GPL-2.0-only
"""Windows archive admission."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import is_sha256
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.runtime.windows_tools import WINDOWS_TOOLS, windows_tool_paths

from .host import fail

if TYPE_CHECKING:
    from pathlib import Path
    from types import ModuleType


@dataclass(frozen=True)
class WindowsBundle:
    """The validated canonical image and its separately packaged native tools."""

    root: Path
    runtime: dict[str, Any]
    tools: dict[str, Path]
    adapter: ModuleType
    generation: str
    device_identity: str


def validate_archive(
    root: Path, runtime: dict[str, Any], *, generation: str, device_identity: str
) -> WindowsBundle:
    """Check files without discovering USB or creating private host state."""
    runner = bundle_runtime
    if runtime["transport"] != "usb-ncm":
        fail("This archive does not support the authenticated Linux endpoint")
    tool_paths = windows_tool_paths(runtime["host_tools"])
    platform_name = runtime["identity"]["platform"]["name"]
    native_roles = {"loader", "bridge"} if platform_name == "ums9117" else {"loader"}
    if any(runtime["host_tools"].get(role) != WINDOWS_TOOLS[role] for role in native_roles):
        fail("The RAM runner does not select this archive's native Windows tools")
    adapter = runner.load_adapter(root / runner.adapter_path(runtime))
    if platform_name == "sc9820e":
        adapter.validate_runtime(runtime)
        if "initramfs" not in runtime["adapter"]["regions"]:
            fail("Windows archives require a RAM root filesystem")
    elif platform_name == "ums9117":
        adapter.runtime_target_display_name(runtime)
        adapter.adapter_config(runtime["adapter"])
        with (root / runtime["image"]).open("rb") as stream:
            if stream.read(4) != b"DHTB":
                fail("The RAM payload does not have its required header")
    else:
        fail("This platform has no Windows RAM loader")
    try:
        windows = json.loads((root / "windows-host.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"Windows host manifest cannot be read: {error}")
    windows = runner.require_object(windows, {"host_tools", "sha256"}, "Windows host manifest")
    if windows["host_tools"] != tool_paths:
        fail("Windows host tools do not match this archive format")
    hashes = windows["sha256"]
    required_files = set(tool_paths.values())
    if not isinstance(hashes, dict) or not required_files.issubset({*runtime["sha256"], *hashes}):
        fail("Windows host manifest lacks native tool checksums")
    for relative, expected in hashes.items():
        name = runner.relative_name(relative, "Windows file")
        if not isinstance(expected, str) or not is_sha256(expected):
            fail(f"Windows file checksum is invalid: {name}")
        runner.require_file(root / name)
        if runner.digest(root / name) != expected:
            fail(f"Windows file checksum differs: {name}")
    return WindowsBundle(
        root,
        runtime,
        {role: root / relative for role, relative in tool_paths.items()},
        adapter,
        generation,
        device_identity,
    )
