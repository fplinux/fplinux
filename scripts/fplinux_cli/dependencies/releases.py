# SPDX-License-Identifier: GPL-2.0-only
"""Restore a public release's exact originals without private credentials."""

from __future__ import annotations

import re
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.artifacts.release import capture_public_sources
from fplinux_cli.common import fail
from fplinux_cli.dependencies.distribution import DEPENDENCY_METADATA, dependency_metadata
from fplinux_cli.dependencies.transport import transport_records
from fplinux_cli.dependencies.workflow import import_dependencies, restore_dependencies
from fplinux_cli.environment.downloads import download_locked_file, transfer_verified_file


def restore_release(release_id: str) -> None:
    """Select an explicit matching Release, authenticate its parts, and restore inputs."""
    if re.fullmatch(r"[0-9a-f]{12}", release_id) is None:
        fail("dependency restoration requires a twelve-character release ID")
    expected = capture_public_sources().release
    if expected.release_id != release_id:
        fail("selected release does not match this source checkout")
    cache = common.ROOT / ".cache"
    cache.mkdir(exist_ok=True)
    base = f"https://github.com/fplinux/fplinux/releases/download/{expected.tag}/"
    with tempfile.TemporaryDirectory(prefix=".release-download-", dir=cache) as temporary:
        work = Path(temporary)
        parts = work / "parts"
        parts.mkdir()

        def verify_metadata(path: Path) -> None:
            if dependency_metadata(path.read_bytes()).release != expected:
                fail("published dependency metadata belongs to another source release")

        metadata_path = transfer_verified_file(
            base + DEPENDENCY_METADATA,
            parts / DEPENDENCY_METADATA,
            verify=verify_metadata,
        )
        metadata = dependency_metadata(metadata_path.read_bytes())
        download_locked_file(base + metadata.index, metadata.index_sha256, parts / metadata.index)
        for digest, name in transport_records(
            parts, index_name=metadata.index, index_sha256=metadata.index_sha256
        ):
            download_locked_file(base + name, digest, parts / name)
        snapshot = work / "snapshot"
        import_dependencies(parts, snapshot)
        restore_dependencies(snapshot, inputs_only=True)
