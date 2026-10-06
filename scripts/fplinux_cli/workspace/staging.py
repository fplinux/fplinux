# SPDX-License-Identifier: GPL-2.0-only
"""Materialize and discard verified disposable source workspaces."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path, PurePosixPath

from fplinux_cli.common import ROOT, fail

from .capture import WorkspaceFile, WorkspaceSnapshot, snapshot_recipe, workspace_relative_path


def stage_workspace_snapshot(snapshot: WorkspaceSnapshot) -> Path:
    """Materialize one previously captured target workspace snapshot on a cache miss."""
    return _stage_snapshot(
        snapshot,
        ROOT / ".cache/workspaces",
        PurePosixPath(".fplinux-workspace"),
    )


def stage_quality_workspace_snapshot(snapshot: WorkspaceSnapshot) -> Path:
    """Materialize one previously captured quality workspace snapshot on a cache miss."""
    return _stage_snapshot(
        snapshot,
        ROOT / ".cache/quality-workspaces",
        PurePosixPath(".cache/.fplinux-workspace"),
    )


def discard_staged_workspace_snapshot(snapshot: WorkspaceSnapshot, workspace: Path) -> None:
    """Remove only the exact completed target workspace addressed by one snapshot."""
    _discard_staged_snapshot(
        snapshot,
        workspace,
        ROOT / ".cache/workspaces",
        PurePosixPath(".fplinux-workspace"),
    )


def discard_staged_quality_workspace_snapshot(
    snapshot: WorkspaceSnapshot, workspace: Path
) -> None:
    """Remove only the exact completed quality workspace addressed by one snapshot."""
    _discard_staged_snapshot(
        snapshot,
        workspace,
        ROOT / ".cache/quality-workspaces",
        PurePosixPath(".cache/.fplinux-workspace"),
    )


def _stage_snapshot(
    snapshot: WorkspaceSnapshot,
    workspaces: Path,
    marker_relative: PurePosixPath,
) -> Path:
    """Publish one captured snapshot; a mismatched cache entry is a plain miss."""
    _validate_snapshot(snapshot, marker_relative)
    _managed_workspace_namespace(workspaces, create=True)
    recipe = snapshot.materialization_recipe
    workspace = workspaces / recipe
    marker = workspace / marker_relative
    try:
        if (
            not workspace.is_symlink()
            and workspace.is_dir()
            and not marker.is_symlink()
            and marker.read_text(encoding="utf-8").strip() == recipe
        ):
            return workspace
    except OSError:
        pass

    if workspace.exists() or workspace.is_symlink():
        _remove_managed_workspace(workspaces, workspace, "stale workspace")

    staging = Path(tempfile.mkdtemp(dir=workspaces, prefix=f".stage-{recipe[:12]}-"))
    try:
        for source in snapshot.materialized_files:
            _write_snapshot_file(staging / source.path, source)
        staged_marker = staging / marker_relative
        staged_marker.parent.mkdir(parents=True, exist_ok=True)
        staged_marker.write_bytes((recipe + "\n").encode())
        staging.replace(workspace)
        return workspace
    finally:
        if staging.exists():
            _remove_managed_workspace(workspaces, staging, "workspace staging directory")


def _discard_staged_snapshot(
    snapshot: WorkspaceSnapshot,
    workspace: Path,
    workspaces: Path,
    marker_relative: PurePosixPath,
) -> None:
    """Discard one validated disposable workspace without accepting alternate paths."""
    _validate_snapshot(snapshot, marker_relative)
    _managed_workspace_namespace(workspaces, create=False)
    recipe = snapshot.materialization_recipe
    expected = workspaces / recipe
    if workspace != expected:
        fail(f"workspace discard path is outside its managed cache slot: {workspace}")
    marker = workspace / marker_relative
    expected_marker = (recipe + "\n").encode()
    try:
        if workspace.is_symlink() or not workspace.is_dir():
            fail(f"workspace discard path is missing or invalid: {workspace}")
        marker_parent = workspace
        for component in marker_relative.parts[:-1]:
            marker_parent /= component
            if marker_parent.is_symlink() or not marker_parent.is_dir():
                fail(f"workspace discard marker parent is missing or invalid: {marker_parent}")
        if marker.is_symlink() or not marker.is_file() or marker.read_bytes() != expected_marker:
            fail(f"workspace discard marker is missing or invalid: {marker}")
        _remove_managed_workspace(workspaces, workspace, "workspace discard path")
    except OSError as error:
        fail(f"workspace discard failed: {workspace}: {error}")


def _managed_workspace_namespace(workspaces: Path, *, create: bool) -> None:
    """Require the two managed cache components to be real directories."""
    cache = ROOT / ".cache"
    if workspaces not in {cache / "workspaces", cache / "quality-workspaces"}:
        fail(f"workspace namespace is outside the managed cache: {workspaces}")
    _managed_directory(cache, "workspace cache root", create=create)
    _managed_directory(workspaces, "workspace namespace", create=create)


def _managed_directory(path: Path, name: str, *, create: bool) -> None:
    """Require one cache component to be a real directory, optionally creating it."""
    try:
        if path.is_symlink():
            fail(f"{name} is missing or invalid: {path}")
        if path.is_dir():
            return
        if path.exists() or not create:
            fail(f"{name} is missing or invalid: {path}")
        path.mkdir()
        if path.is_symlink() or not path.is_dir():
            fail(f"{name} is missing or invalid: {path}")
    except OSError as error:
        fail(f"{name} cannot be prepared: {path}: {error}")


def _remove_managed_workspace(workspaces: Path, workspace: Path, name: str) -> None:
    """Revalidate cache components and one real child immediately before removal."""
    _managed_workspace_namespace(workspaces, create=False)
    if workspace.parent != workspaces or workspace.is_symlink() or not workspace.is_dir():
        fail(f"{name} is missing or invalid: {workspace}")
    try:
        shutil.rmtree(workspace)
    except OSError as error:
        fail(f"{name} cannot be removed: {workspace}: {error}")


def _validate_snapshot(snapshot: WorkspaceSnapshot, marker_relative: PurePosixPath) -> None:
    """Reject forged snapshots before they can create cache paths outside the namespace."""
    expected_recipe = snapshot_recipe(
        snapshot.files, snapshot.build_type, snapshot.linux_operations, snapshot.alpine_graph
    )
    if snapshot.recipe != expected_recipe:
        fail("workspace snapshot recipe does not match its files")
    marker = workspace_relative_path(marker_relative.as_posix())
    paths = [source.path for source in snapshot.materialized_files]
    if len(paths) != len(set(paths)):
        fail("workspace snapshot contains duplicate paths")
    for source in snapshot.materialized_files:
        if workspace_relative_path(source.path) != source.path:
            fail(f"workspace snapshot has invalid path: {source.path}")
        if not isinstance(source.contents, bytes):
            fail(f"workspace snapshot has non-bytes contents: {source.path}")
        if type(source.mode) is not int or not 0 <= source.mode <= 0o777:
            fail(f"workspace snapshot has invalid mode: {source.path}")
    if marker in paths:
        fail("workspace snapshot collides with its recipe marker")


def _write_snapshot_file(destination: Path, source: WorkspaceFile) -> None:
    """Materialize exact snapshot bytes and mode, without reading the source checkout."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    written = destination.write_bytes(source.contents)
    if written != len(source.contents):
        fail(f"could not write complete workspace file: {destination}")
    destination.chmod(source.mode)
