# SPDX-License-Identifier: GPL-2.0-only
"""Restore a public release's exact originals without private credentials."""

from __future__ import annotations

import re
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING
from urllib.error import HTTPError

from fplinux_cli import common
from fplinux_cli.artifacts.release import capture_public_sources
from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.dependencies.distribution import DEPENDENCY_METADATA, dependency_metadata
from fplinux_cli.dependencies.transport import transport_records
from fplinux_cli.dependencies.workflow import import_dependencies, restore_dependencies
from fplinux_cli.environment.downloads import download_locked_file, transfer_verified_file

if TYPE_CHECKING:
    from fplinux_cli.artifacts.release_identity import ReleaseContext
    from fplinux_cli.dependencies.distribution import DependencyMetadata


class _DifferentReleaseError(Exception):
    """The transport identifies a different build sharing the public version."""


def _download_metadata(expected: ReleaseContext, parts: Path) -> tuple[str, DependencyMetadata]:
    def verify_metadata(path: Path) -> None:
        if dependency_metadata(path.read_bytes()).release != expected:
            raise _DifferentReleaseError

    different_release = False
    for tag in (expected.stable_tag, expected.build_tag):
        base = f"https://github.com/fplinux/fplinux/releases/download/{tag}/"
        try:
            metadata_path = transfer_verified_file(
                base + DEPENDENCY_METADATA,
                parts / DEPENDENCY_METADATA,
                verify=verify_metadata,
            )
        except HTTPError as error:
            if error.code != 404:
                raise
        except _DifferentReleaseError:
            different_release = True
        else:
            return base, dependency_metadata(metadata_path.read_bytes())
    if different_release:
        fail("published dependency metadata belongs to another source release")
    return fail("no published dependency transport was found for this source release")


def restore_release(release_id: str) -> None:
    """Select an explicit matching Release, authenticate its parts, and restore inputs."""
    if re.fullmatch(r"[0-9a-f]{12}", release_id) is None:
        fail("dependency restoration requires a twelve-character release ID")
    expected = capture_public_sources().release
    if expected.release_id != release_id:
        fail("selected release does not match this source checkout")
    cache = data_root(common.ROOT)
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-download-", dir=cache) as temporary:
        work = Path(temporary)
        parts = work / "parts"
        parts.mkdir()

        base, metadata = _download_metadata(expected, parts)
        download_locked_file(base + metadata.index, metadata.index_sha256, parts / metadata.index)
        for digest, name in transport_records(
            parts, index_name=metadata.index, index_sha256=metadata.index_sha256
        ):
            download_locked_file(base + name, digest, parts / name)
        snapshot = work / "snapshot"
        import_dependencies(parts, snapshot)
        restore_dependencies(snapshot, inputs_only=True)
