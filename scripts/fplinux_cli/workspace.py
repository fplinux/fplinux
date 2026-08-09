# SPDX-License-Identifier: GPL-2.0-only
"""Create immutable content-addressed source workspaces."""

from __future__ import annotations

import hashlib
import shutil
import tempfile
from pathlib import Path

from .common import ROOT, fail, sha256_file
from .config import load_platform, load_target


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
        fail(f"workspace input is missing: {path}")
    for child in sorted(path.rglob("*")):
        relative_child = child.relative_to(ROOT)
        if is_python_cache(child):
            continue
        if child.is_symlink():
            fail(f"workspace input must not be a symlink: {child}")
        if child.is_file():
            files[relative_child.as_posix()] = child


def target_build_source_files(target: str) -> list[tuple[str, Path]]:
    """Resolve only the selected target/platform build closure."""
    target_config = load_target(target)
    platform = load_platform(target_config["platform"])
    target_root = ROOT / "targets" / target
    files: dict[str, Path] = {}

    for relative in (
        "THIRD_PARTY_NOTICES.md",
        "sources.lock.toml",
        "buildroot-external",
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/builder.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/config.py",
        "scripts/fplinux_cli/output.py",
    ):
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
    """Return the complete source closure used by quality checks."""
    files: list[tuple[str, Path]] = []
    for top in sorted(ROOT.iterdir()):
        if top.name in {".cache", ".git"} or is_python_cache(top):
            continue
        if top.is_symlink():
            if enforce_source_policy:
                fail(f"quality input must not be a symlink: {top}")
            continue
        if top.is_file():
            files.append((top.name, top))
            continue
        if not top.is_dir():
            continue
        for child in sorted(top.rglob("*")):
            if is_python_cache(child):
                continue
            if child.is_symlink():
                if enforce_source_policy:
                    fail(f"quality input must not be a symlink: {child}")
                continue
            if child.is_file():
                files.append((child.relative_to(ROOT).as_posix(), child))
    return files


def workspace_matches(
    workspace: Path,
    marker: Path,
    recipe: str,
    files: list[tuple[str, Path]],
) -> bool:
    """Verify a cached immutable source workspace."""
    if not marker.is_file() or marker.is_symlink() or marker.read_text().strip() != recipe:
        return False
    expected = {relative for relative, _source in files}
    actual: set[str] = set()
    for child in workspace.rglob("*"):
        if child.is_symlink():
            return False
        if not child.is_file() or child == marker:
            continue
        actual.add(child.relative_to(workspace).as_posix())
    if actual != expected:
        return False
    for relative, source in files:
        staged = workspace / relative
        if (staged.stat().st_mode & 0o777) != (source.stat().st_mode & 0o777):
            return False
        if sha256_file(staged) != sha256_file(source):
            return False
    return True


def workspace_recipe(files: list[tuple[str, Path]]) -> str:
    """Hash source paths, bytes and executable modes."""
    value = hashlib.sha256()
    for relative, source in files:
        value.update(relative.encode())
        value.update(b"\0")
        value.update(source.read_bytes())
        value.update((source.stat().st_mode & 0o777).to_bytes(2, "big"))
        value.update(b"\0")
    return value.hexdigest()


def stage_workspace(target: str) -> Path:
    """Stage an immutable build closure for one selected target."""
    files = target_build_source_files(target)
    recipe = workspace_recipe(files)
    workspaces = ROOT / ".cache/workspaces"
    workspace = workspaces / recipe
    marker = workspace / ".fplinux-workspace"
    if workspace.is_dir():
        if workspace_matches(workspace, marker, recipe, files):
            return workspace
        fail(f"cached staged workspace failed integrity verification: {workspace}")
    if workspace.exists():
        fail(f"incomplete staged workspace: {workspace}")

    workspaces.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(dir=workspaces, prefix=f".stage-{recipe[:12]}-"))
    for relative, source in files:
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    (staging / ".fplinux-workspace").write_text(recipe + "\n")
    staging.replace(workspace)
    return workspace


def stage_quality_workspace(*, enforce_source_policy: bool) -> Path:
    """Stage the complete source tree for hermetic quality tools."""
    files = quality_files(enforce_source_policy=enforce_source_policy)
    recipe = workspace_recipe(files)
    workspaces = ROOT / ".cache/quality-workspaces"
    workspace = workspaces / recipe
    marker = workspace / ".cache/.fplinux-workspace"
    if workspace.is_dir():
        if workspace_matches(workspace, marker, recipe, files):
            return workspace
        fail(f"cached quality workspace failed integrity verification: {workspace}")
    if workspace.exists():
        fail(f"incomplete staged workspace: {workspace}")

    workspaces.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(dir=workspaces, prefix=f".stage-{recipe[:12]}-"))
    for relative, source in files:
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    marker = staging / ".cache/.fplinux-workspace"
    marker.parent.mkdir(parents=True)
    marker.write_text(recipe + "\n")
    staging.replace(workspace)
    return workspace
