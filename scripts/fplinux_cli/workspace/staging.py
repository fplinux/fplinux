# SPDX-License-Identifier: GPL-2.0-only
"""Materialize and discard verified disposable source workspaces."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

from fplinux_cli.artifacts.release import BUILD_IDENTITY_NAME
from fplinux_cli.common import ROOT, canonical_json_bytes, fail
from fplinux_cli.data_paths import data_root

from .capture import WorkspaceFile, WorkspaceSnapshot, snapshot_recipe, workspace_relative_path

_WORKSPACE_MARKER = ".fplinux-workspace"


def stage_workspace_snapshot(
    snapshot: WorkspaceSnapshot, *, selected_data_root: Path | None = None
) -> Path:
    """Materialize one previously captured target workspace snapshot on a cache miss."""
    cache = data_root(ROOT) if selected_data_root is None else selected_data_root
    return _stage_snapshot(snapshot, cache, cache / "workspaces")


def stage_quality_workspace_snapshot(
    snapshot: WorkspaceSnapshot, *, selected_data_root: Path | None = None
) -> Path:
    """Materialize one previously captured quality workspace snapshot on a cache miss."""
    cache = data_root(ROOT) if selected_data_root is None else selected_data_root
    return _stage_snapshot(snapshot, cache, cache / "quality-workspaces")


def discard_staged_workspace_snapshot(
    snapshot: WorkspaceSnapshot, workspace: Path, *, selected_data_root: Path | None = None
) -> None:
    """Remove only the exact completed target workspace addressed by one snapshot."""
    cache = data_root(ROOT) if selected_data_root is None else selected_data_root
    _discard_staged_snapshot(snapshot, workspace, cache, cache / "workspaces")


def discard_staged_quality_workspace_snapshot(
    snapshot: WorkspaceSnapshot, workspace: Path, *, selected_data_root: Path | None = None
) -> None:
    """Remove only the exact completed quality workspace addressed by one snapshot."""
    cache = data_root(ROOT) if selected_data_root is None else selected_data_root
    _discard_staged_snapshot(snapshot, workspace, cache, cache / "quality-workspaces")


def _stage_snapshot(
    snapshot: WorkspaceSnapshot,
    cache: Path,
    workspaces: Path,
) -> Path:
    """Publish one captured snapshot; a mismatched cache entry is a plain miss."""
    _validate_snapshot(snapshot)
    _managed_workspace_namespace(cache, workspaces, create=True)
    recipe = snapshot.materialization_recipe
    workspace = workspaces / recipe
    marker = workspace / _WORKSPACE_MARKER
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
        _remove_managed_workspace(cache, workspaces, workspace, "stale workspace")

    staging = Path(tempfile.mkdtemp(dir=workspaces, prefix=f".stage-{recipe[:12]}-"))
    try:
        for source in snapshot.materialized_files:
            _write_snapshot_file(staging / source.path, source)
        if snapshot.release_context is not None:
            identity = staging / BUILD_IDENTITY_NAME
            identity.write_bytes(canonical_json_bytes(snapshot.release_context.fields()))
            identity.chmod(0o644)
        staged_marker = staging / _WORKSPACE_MARKER
        staged_marker.write_bytes((recipe + "\n").encode())
        staging.replace(workspace)
        return workspace
    finally:
        if staging.exists():
            _remove_managed_workspace(cache, workspaces, staging, "workspace staging directory")


def _discard_staged_snapshot(
    snapshot: WorkspaceSnapshot,
    workspace: Path,
    cache: Path,
    workspaces: Path,
) -> None:
    """Discard one validated disposable workspace without accepting alternate paths."""
    _validate_snapshot(snapshot)
    _managed_workspace_namespace(cache, workspaces, create=False)
    recipe = snapshot.materialization_recipe
    expected = workspaces / recipe
    if workspace != expected:
        fail(f"workspace discard path is outside its managed cache slot: {workspace}")
    marker = workspace / _WORKSPACE_MARKER
    expected_marker = (recipe + "\n").encode()
    try:
        if workspace.is_symlink() or not workspace.is_dir():
            fail(f"workspace discard path is missing or invalid: {workspace}")
        if marker.is_symlink() or not marker.is_file() or marker.read_bytes() != expected_marker:
            fail(f"workspace discard marker is missing or invalid: {marker}")
        _remove_managed_workspace(cache, workspaces, workspace, "workspace discard path")
    except OSError as error:
        fail(f"workspace discard failed: {workspace}: {error}")


def _managed_workspace_namespace(cache: Path, workspaces: Path, *, create: bool) -> None:
    """Require the two managed cache components to be real directories."""
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


def _remove_managed_workspace(cache: Path, workspaces: Path, workspace: Path, name: str) -> None:
    """Revalidate cache components and one real child immediately before removal."""
    _managed_workspace_namespace(cache, workspaces, create=False)
    if workspace.parent != workspaces or workspace.is_symlink() or not workspace.is_dir():
        fail(f"{name} is missing or invalid: {workspace}")
    try:
        shutil.rmtree(workspace)
    except OSError as error:
        fail(f"{name} cannot be removed: {workspace}: {error}")


def _validate_snapshot(snapshot: WorkspaceSnapshot) -> None:
    """Reject forged snapshots before they can create cache paths outside the namespace."""
    expected_recipe = snapshot_recipe(
        snapshot.files,
        snapshot.build_type,
        snapshot.linux_operations,
        snapshot.alpine_graph,
        snapshot.source_declarations,
    )
    if snapshot.recipe != expected_recipe:
        fail("workspace snapshot recipe does not match its files")
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
    if _WORKSPACE_MARKER in paths or BUILD_IDENTITY_NAME in paths:
        fail("workspace snapshot collides with its recipe marker")


def _write_snapshot_file(destination: Path, source: WorkspaceFile) -> None:
    """Materialize exact snapshot bytes and mode, without reading the source checkout."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    written = destination.write_bytes(source.contents)
    if written != len(source.contents):
        fail(f"could not write complete workspace file: {destination}")
    destination.chmod(source.mode)
