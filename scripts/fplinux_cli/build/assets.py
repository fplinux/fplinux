# SPDX-License-Identifier: GPL-2.0-only
"""Prepare pinned loader assets for a target bundle."""

from __future__ import annotations

import shutil
import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.inputs import fail
from fplinux_cli.common import replace_file_atomically, sha256_bytes
from fplinux_cli.manifests.assets import load_asset_lock

if TYPE_CHECKING:
    from pathlib import Path


def extract_7z_member(archive: Path, member: str) -> bytes:
    """Extract exactly one declared 7z member without a shell."""
    executable = shutil.which("7z") or shutil.which("7zz")
    if executable is None:
        fail("7z or 7zz is required")
    result = subprocess.run(
        [executable, "x", "-so", str(archive), member],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        fail(result.stderr.decode(errors="replace").strip())
    return result.stdout


def write_checked(data: bytes, destination: Path, expected: str) -> None:
    """Atomically write bytes that match their declared digest."""
    actual = sha256_bytes(data)
    if actual != expected:
        fail(f"{destination.name} SHA256 mismatch: expected {expected}, got {actual}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(destination, data, 0o600, sync=False)


def build_assets(lock_path: Path, output: Path) -> dict[str, tuple[str, str]]:
    """Fetch and extract every pinned asset through typed extractors."""
    result: dict[str, tuple[str, str]] = {}
    for source in load_asset_lock(lock_path):
        archive = sources_build.fetch(
            source["url"],
            source["sha256"],
            inputs_build.CACHE / "downloads",
            source["cache_name"],
        )
        for item in source["output"]:
            expected = item["sha256"]
            data = (
                archive.read_bytes()
                if source["kind"] == "file"
                else extract_7z_member(archive, item["member"])
            )
            destination = output / item["path"]
            write_checked(data, destination, expected)
            result[item["role"]] = (item["path"], expected)
            process_build.log_message(f"{expected}  {destination}")
    return result
