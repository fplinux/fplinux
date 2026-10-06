# SPDX-License-Identifier: GPL-2.0-only
"""Capture exact immutable source bytes, modes and causal recipes."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import canonical_json_bytes, fail, relative_name

if TYPE_CHECKING:
    from collections.abc import Callable


@dataclass(frozen=True)
class WorkspaceFile:
    """One immutable regular file in a workspace snapshot."""

    path: str
    contents: bytes
    mode: int


@dataclass(frozen=True)
class WorkspaceSnapshot:
    """A causal recipe and the additional sources needed to execute its build."""

    files: tuple[WorkspaceFile, ...]
    recipe: str
    build_type: str | None = None
    auxiliary_files: tuple[WorkspaceFile, ...] = ()
    linux_operations: tuple[tuple[str, str, str], ...] = ()
    alpine_graph: bytes = b""

    @property
    def materialization_recipe(self) -> str:
        """Bind the disposable workspace slot to every file it contains."""
        return snapshot_recipe(
            self.materialized_files,
            self.build_type,
            self.linux_operations,
            self.alpine_graph,
        )

    @property
    def materialized_files(self) -> tuple[WorkspaceFile, ...]:
        """Combine causal and auxiliary inputs in one deterministic projection."""
        return tuple(sorted((*self.files, *self.auxiliary_files), key=lambda item: item.path))


def workspace_snapshot(files: list[tuple[str, Path]]) -> WorkspaceSnapshot:
    """Capture one stable immutable snapshot from an already resolved file list."""
    source_files = _normalize_source_files(files)
    return snapshot_from_inventory(lambda: list(source_files))


def snapshot_from_inventory(
    inventory: Callable[[], list[tuple[str, Path]]],
) -> WorkspaceSnapshot:
    """Read one exact source inventory into an immutable in-memory snapshot."""
    source_files = _normalize_source_files(inventory())
    snapshot_files = tuple(read_source_file(relative, source) for relative, source in source_files)
    return WorkspaceSnapshot(snapshot_files, snapshot_recipe(snapshot_files))


def _normalize_source_files(files: list[tuple[str, Path]]) -> tuple[tuple[str, Path], ...]:
    """Require one deterministic, non-overlapping set of relative source paths."""
    normalized: list[tuple[str, Path]] = []
    seen: set[str] = set()
    for relative, source in files:
        normalized_relative = workspace_relative_path(relative)
        if normalized_relative in seen:
            fail(f"workspace input path is duplicated: {normalized_relative}")
        seen.add(normalized_relative)
        normalized.append((normalized_relative, Path(source)))
    return tuple(sorted(normalized))


def workspace_relative_path(value: str) -> str:
    """Reject paths that could escape a staged workspace."""
    normalized = relative_name(value, field="workspace input path")
    path = PurePosixPath(normalized)
    if path == PurePosixPath("."):
        fail("workspace input path must name a regular file")
    return normalized


def read_source_file(relative: str, source: Path) -> WorkspaceFile:
    """Read one regular source file into a workspace snapshot."""
    if source.is_symlink() or not source.is_file():
        fail(f"workspace input must be a regular file: {source}")
    try:
        contents = source.read_bytes()
        mode = source.stat().st_mode & 0o777
    except OSError as error:
        fail(f"workspace input cannot be read: {source}: {error}")
    return WorkspaceFile(relative, contents, mode)


def snapshot_recipe(
    files: tuple[WorkspaceFile, ...],
    build_type: str | None = None,
    linux_operations: tuple[tuple[str, str, str], ...] = (),
    alpine_graph: bytes = b"",
) -> str:
    """Hash exact source paths, bytes, and permissions from an immutable snapshot."""
    value = hashlib.sha256()
    if build_type is not None:
        value.update(f"build_type={build_type}\0".encode())
    if linux_operations:
        value.update(b"linux_operations\0")
        value.update(canonical_json_bytes(linux_operations))
        value.update(b"\0")
    if alpine_graph:
        value.update(b"alpine_graph\0")
        value.update(alpine_graph)
        value.update(b"\0")
    for source in files:
        value.update(source.path.encode())
        value.update(b"\0")
        value.update(source.contents)
        value.update(source.mode.to_bytes(2, "big"))
        value.update(b"\0")
    return value.hexdigest()
