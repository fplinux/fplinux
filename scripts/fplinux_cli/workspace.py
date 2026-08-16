# SPDX-License-Identifier: GPL-2.0-only
"""Create immutable content-addressed source workspaces."""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable

from .common import ROOT, fail, relative_name
from .config import load_platform, load_target

STAGED_BUILD_SOURCES = (
    "Containerfile",
    "THIRD_PARTY_NOTICES.md",
    "container.lock.toml",
    "sources.lock.toml",
    "buildroot-external",
    "scripts/fplinux_cli/__init__.py",
    "scripts/fplinux_cli/buildroot_state.py",
    "scripts/fplinux_cli/builder.py",
    "scripts/fplinux_cli/bundle_state.py",
    "scripts/fplinux_cli/common.py",
    "scripts/fplinux_cli/config.py",
    "scripts/fplinux_cli/device_state.py",
    "scripts/fplinux_cli/kbuild_state.py",
    "scripts/fplinux_cli/linux_state.py",
    "scripts/fplinux_cli/output.py",
    "scripts/fplinux_cli/toolchain_state.py",
)


@dataclass(frozen=True)
class WorkspaceFile:
    """One immutable regular file in a workspace snapshot."""

    path: str
    contents: bytes
    mode: int


@dataclass(frozen=True)
class WorkspaceSnapshot:
    """The exact causal input used to validate or materialize a workspace."""

    files: tuple[WorkspaceFile, ...]
    recipe: str


def is_python_cache(path: Path) -> bool:
    """Return whether a repository path is generated Python bytecode."""
    relative = path.relative_to(ROOT)
    return "__pycache__" in relative.parts or path.suffix in {".pyc", ".pyo"}


def add_source_path(files: dict[str, Path], path: Path) -> None:
    """Add a regular file or a complete source directory to the closure."""
    if path.is_symlink():
        fail(f"workspace input must not be a symlink: {path}")
    if path.is_file():
        relative = path.relative_to(ROOT).as_posix()
        if not is_python_cache(path):
            files[relative] = path
        return
    if not path.is_dir():
        fail(f"workspace input is missing or not a directory: {path}")
    for child in sorted(path.rglob("*")):
        if is_python_cache(child):
            continue
        if child.is_symlink():
            fail(f"workspace input must not be a symlink: {child}")
        if child.is_dir():
            continue
        if not child.is_file():
            fail(f"workspace input must be a regular file: {child}")
        files[child.relative_to(ROOT).as_posix()] = child


def target_build_source_files(target: str) -> list[tuple[str, Path]]:
    """Resolve only the selected target/platform build closure."""
    target_config = load_target(target)
    platform = load_platform(target_config["platform"])
    target_root = ROOT / "targets" / target
    files: dict[str, Path] = {}

    for relative in STAGED_BUILD_SOURCES:
        add_source_path(files, ROOT / relative)
    add_source_path(files, target_root / "target.toml")
    add_source_path(files, target_root / target_config["release_manifest"])
    add_source_path(files, target_root / target_config["assets_lock"])
    add_source_path(files, target_root / target_config["buildroot"]["defconfig"])
    add_source_path(files, (target_root / target_config["buildroot"]["defconfig"]).parent)
    add_source_path(files, target_root / target_config["linux"]["defconfig"])
    add_source_path(files, target_root / target_config["bootstrap"]["source"])
    for relative in target_config["linux"]["patches"]:
        add_source_path(files, target_root / relative)
    for key in ("copies", "appends"):
        for step in target_config["linux"][key]:
            add_source_path(files, target_root / step["source"])

    platform_root = ROOT / "platforms" / target_config["platform"]
    add_source_path(files, platform_root / "platform.toml")
    add_source_path(files, ROOT / platform["buildroot"]["toolchain_defconfig"])
    add_source_path(files, ROOT / platform["buildroot"]["toolchain_external_defconfig"])
    for relative in platform["buildroot"]["shared_paths"]:
        add_source_path(files, ROOT / relative)
    for relative in platform["linux"]["patches"]:
        add_source_path(files, ROOT / relative)
    for key in ("copies", "appends"):
        for step in platform["linux"][key]:
            add_source_path(files, ROOT / step["source"])
    for step in platform["bootstrap"]["shared_copies"]:
        add_source_path(files, ROOT / step["source"])
    for recipe in platform["host"]["tools"]:
        if recipe["type"] == "cc-libusb/v1":
            add_source_path(files, ROOT / recipe["source"])
    add_source_path(files, ROOT / "common/run.py")
    add_source_path(files, platform_root / "host/adapter.py")
    return sorted(files.items())


def quality_files(*, enforce_source_policy: bool) -> list[tuple[str, Path]]:
    """Return tracked and non-ignored untracked files used by quality checks."""
    result = subprocess.run(
        [
            "git",
            "-C",
            str(ROOT),
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        fail(f"cannot inventory Git source files: {detail or 'git ls-files failed'}")
    files: list[tuple[str, Path]] = []
    for encoded in sorted(filter(None, result.stdout.split(b"\0"))):
        relative_text = os.fsdecode(encoded)
        relative = PurePosixPath(relative_text)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or relative.as_posix() != relative_text
        ):
            fail(f"Git returned an unsafe source path: {relative_text}")
        path = ROOT / relative_text
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            # A tracked deletion is a valid checkout state and contributes no bytes.
            continue
        if path.suffix in {".pyc", ".pyo"} or "__pycache__" in relative.parts:
            if enforce_source_policy:
                fail(f"generated Python cache is not allowed in source: {relative_text}")
            continue
        if stat.S_ISLNK(metadata.st_mode):
            if enforce_source_policy:
                fail(f"quality input must not be a symlink: {path}")
            continue
        if not stat.S_ISREG(metadata.st_mode):
            if enforce_source_policy:
                fail(f"quality input must be a regular file: {path}")
            continue
        files.append((relative_text, path))
    return files


def target_workspace_snapshot(target: str) -> WorkspaceSnapshot:
    """Read the selected build closure before deciding whether staging is needed."""
    return _snapshot_from_inventory(lambda: target_build_source_files(target))


def quality_workspace_snapshot(*, enforce_source_policy: bool) -> WorkspaceSnapshot:
    """Read the complete quality closure before deciding whether staging is needed."""
    return _snapshot_from_inventory(
        lambda: quality_files(enforce_source_policy=enforce_source_policy)
    )


def workspace_snapshot(files: list[tuple[str, Path]]) -> WorkspaceSnapshot:
    """Capture one stable immutable snapshot from an already resolved file list."""
    source_files = _normalize_source_files(files)
    return _snapshot_from_inventory(lambda: list(source_files))


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


def _snapshot_from_inventory(
    inventory: Callable[[], list[tuple[str, Path]]],
) -> WorkspaceSnapshot:
    """Read one exact source inventory into an immutable in-memory snapshot."""
    source_files = _normalize_source_files(inventory())
    snapshot_files = tuple(
        _read_source_file(relative, source) for relative, source in source_files
    )
    return WorkspaceSnapshot(snapshot_files, _snapshot_recipe(snapshot_files))


def _normalize_source_files(files: list[tuple[str, Path]]) -> tuple[tuple[str, Path], ...]:
    """Require one deterministic, non-overlapping set of relative source paths."""
    normalized: list[tuple[str, Path]] = []
    seen: set[str] = set()
    for relative, source in files:
        normalized_relative = _workspace_relative_path(relative)
        if normalized_relative in seen:
            fail(f"workspace input path is duplicated: {normalized_relative}")
        seen.add(normalized_relative)
        normalized.append((normalized_relative, Path(source)))
    return tuple(sorted(normalized))


def _workspace_relative_path(value: str) -> str:
    """Reject paths that could escape a staged workspace."""
    normalized = relative_name(value, field="workspace input path")
    path = PurePosixPath(normalized)
    if path == PurePosixPath("."):
        fail("workspace input path must name a regular file")
    return normalized


def _read_source_file(relative: str, source: Path) -> WorkspaceFile:
    """Read one regular source file into a workspace snapshot."""
    if source.is_symlink() or not source.is_file():
        fail(f"workspace input must be a regular file: {source}")
    try:
        contents = source.read_bytes()
        mode = source.stat().st_mode & 0o777
    except OSError as error:
        fail(f"workspace input cannot be read: {source}: {error}")
    return WorkspaceFile(relative, contents, mode)


def _snapshot_recipe(files: tuple[WorkspaceFile, ...]) -> str:
    """Hash exact source paths, bytes, and permissions from an immutable snapshot."""
    value = hashlib.sha256()
    for source in files:
        value.update(source.path.encode())
        value.update(b"\0")
        value.update(source.contents)
        value.update(source.mode.to_bytes(2, "big"))
        value.update(b"\0")
    return value.hexdigest()


def _stage_snapshot(
    snapshot: WorkspaceSnapshot,
    workspaces: Path,
    marker_relative: PurePosixPath,
) -> Path:
    """Publish one captured snapshot; a mismatched cache entry is a plain miss."""
    _validate_snapshot(snapshot, marker_relative)
    workspace = workspaces / snapshot.recipe
    marker = workspace / marker_relative
    try:
        if workspace.is_dir() and marker.read_text(encoding="utf-8").strip() == snapshot.recipe:
            return workspace
    except OSError:
        pass
    if workspace.is_dir():
        shutil.rmtree(workspace)
    elif workspace.exists() or workspace.is_symlink():
        workspace.unlink()

    workspaces.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(dir=workspaces, prefix=f".stage-{snapshot.recipe[:12]}-"))
    try:
        for source in snapshot.files:
            _write_snapshot_file(staging / source.path, source)
        staged_marker = staging / marker_relative
        staged_marker.parent.mkdir(parents=True, exist_ok=True)
        staged_marker.write_bytes((snapshot.recipe + "\n").encode())
        staging.replace(workspace)
        return workspace
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def _validate_snapshot(snapshot: WorkspaceSnapshot, marker_relative: PurePosixPath) -> None:
    """Reject forged snapshots before they can create cache paths outside the namespace."""
    expected_recipe = _snapshot_recipe(snapshot.files)
    if snapshot.recipe != expected_recipe:
        fail("workspace snapshot recipe does not match its files")
    marker = _workspace_relative_path(marker_relative.as_posix())
    paths = [source.path for source in snapshot.files]
    if len(paths) != len(set(paths)):
        fail("workspace snapshot contains duplicate paths")
    for source in snapshot.files:
        if _workspace_relative_path(source.path) != source.path:
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
