# SPDX-License-Identifier: GPL-2.0-only
"""Associate one release with its preserved dependency transport."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli.artifacts.release import ReleaseContext, release_context_from_fields
from fplinux_cli.common import canonical_json_bytes, fail, is_sha256, sha256_file

from .transport import transport_parts, verify_transport

if TYPE_CHECKING:
    from pathlib import Path

DEPENDENCY_METADATA = "DEPENDENCIES.json"


@dataclass(frozen=True)
class DependencyMetadata:
    """A source release associated with one pinned transport checksum index."""

    release: ReleaseContext
    snapshot: str
    index: str
    index_sha256: str


@dataclass(frozen=True)
class DependencyDistribution(DependencyMetadata):
    """A release association with its authenticated transport part inventory."""

    parts: tuple[str, ...]

    @property
    def files(self) -> tuple[str, ...]:
        """List the public assets to copy into the final release directory."""
        return (DEPENDENCY_METADATA, self.index, *self.parts)


def write_dependency_distribution(
    directory: Path, *, release: ReleaseContext, snapshot: str, index: Path
) -> DependencyDistribution:
    """Bind verified snapshot parts to their source release before assembly."""
    if not is_sha256(snapshot):
        fail("dependency snapshot ID is invalid")
    if index.parent != directory or index.name != f"FPLinux-{release.release_id}-inputs.sha256":
        fail("dependency index does not match its release")
    metadata = {
        **release.fields(),
        "snapshot": snapshot,
        "index": index.name,
        "index_sha256": sha256_file(index),
    }
    (directory / DEPENDENCY_METADATA).write_bytes(canonical_json_bytes(metadata))
    return read_dependency_distribution(directory, verify_parts=False)


def dependency_metadata(data: bytes) -> DependencyMetadata:
    """Parse the release association before a downloader follows its index."""
    try:
        metadata = json.loads(data)
    except ValueError as error:
        fail(f"release dependency metadata cannot be read: {error}")
    fields = {
        "release_id",
        "source_commit",
        "source_snapshot",
        "snapshot",
        "index",
        "index_sha256",
    }
    if not isinstance(metadata, dict) or set(metadata) != fields:
        fail("release dependency metadata has unsupported fields")
    release = release_context_from_fields(metadata)
    snapshot, index, index_hash = metadata["snapshot"], metadata["index"], metadata["index_sha256"]
    if not isinstance(snapshot, str) or not is_sha256(snapshot):
        fail("release dependency snapshot ID is invalid")
    if index != f"FPLinux-{release.release_id}-inputs.sha256":
        fail("release dependency index differs from its release ID")
    if not isinstance(index_hash, str) or not is_sha256(index_hash):
        fail("release dependency index SHA-256 is invalid")
    return DependencyMetadata(release, snapshot, index, index_hash)


def read_dependency_distribution(
    directory: Path, *, verify_parts: bool = True
) -> DependencyDistribution:
    """Validate release metadata and its index, checking all part bytes by default."""
    path = directory / DEPENDENCY_METADATA
    if path.is_symlink() or not path.is_file():
        fail("release dependency metadata is missing or invalid")
    metadata = dependency_metadata(path.read_bytes())
    if verify_parts:
        parts = tuple(
            part.name
            for part in verify_transport(
                directory, index_name=metadata.index, index_sha256=metadata.index_sha256
            )
        )
    else:
        parts = transport_parts(
            directory, index_name=metadata.index, index_sha256=metadata.index_sha256
        )
    return DependencyDistribution(
        metadata.release, metadata.snapshot, metadata.index, metadata.index_sha256, parts
    )
