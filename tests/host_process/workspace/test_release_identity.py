# SPDX-License-Identifier: GPL-2.0-only
"""Common release identities from real Git sources and exported source trees."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.release import (
    capture_public_sources,
    current_release_context,
    source_identity_bytes,
)

from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def git(root: Path, *arguments: str) -> str:
    """Run Git against one test-owned checkout with bounded execution."""
    return run_process(
        ["git", "-C", str(root), *arguments],
        name="release source Git operation",
        timeout=10,
        check=True,
    ).stdout.strip()


@pytest.fixture
def source_checkout(tmp_path: Path) -> Path:
    """Own a small public tree with target, test and workflow source inputs."""
    root = tmp_path / "checkout"
    root.mkdir()
    for relative, contents in (
        ("README.md", b"Project\n"),
        ("targets/first/target.toml", b'platform = "first"\n'),
        ("targets/second/target.toml", b'platform = "second"\n'),
        ("tests/check.py", b"assert True\n"),
        (".github/workflows/build.yml", b"name: Build\n"),
        (".githooks/pre-push", b"#!/bin/sh\nexit 0\n"),
        ("site/.gitignore", b"dist/\n"),
    ):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        path.chmod(0o755 if relative == ".githooks/pre-push" else 0o644)
    git(root, "init", "--initial-branch=main")
    git(root, "config", "user.name", "Source Test")
    git(root, "config", "user.email", "source@example.invalid")
    git(root, "config", "commit.gpgsign", "false")
    git(root, "add", ".")
    git(root, "commit", "-m", "Initial source")
    return root


def test_clean_sources_use_full_commit_prefix_for_every_target(source_checkout: Path) -> None:
    """Target and host selection do not enter the prebuild source release identity."""
    commit = git(source_checkout, "rev-parse", "HEAD")
    captured = capture_public_sources(source_checkout)
    assert captured.release.release_id == commit[:12]
    assert captured.release.source_commit == commit
    assert current_release_context(source_checkout) == captured.release


@pytest.mark.parametrize(
    "relative",
    ["targets/second/target.toml", "tests/check.py", ".github/workflows/build.yml"],
)
def test_dirty_public_input_changes_common_content_id(
    source_checkout: Path, relative: str
) -> None:
    """An input outside one selected target still changes the common release ID."""
    clean = capture_public_sources(source_checkout)
    path = source_checkout / relative
    path.write_bytes(path.read_bytes() + b"# changed\n")
    dirty = capture_public_sources(source_checkout)
    assert dirty.release.release_id != clean.release.release_id
    assert dirty.release.release_id == dirty.release.source_snapshot[:12]
    assert dirty.release.source_commit == clean.release.source_commit
    assert capture_public_sources(source_checkout).release == dirty.release


def test_source_mode_changes_common_content_id(source_checkout: Path) -> None:
    """Executable mode is part of both public source export and its content identity."""
    clean = capture_public_sources(source_checkout)
    (source_checkout / ".githooks/pre-push").chmod(0o644)
    dirty = capture_public_sources(source_checkout)
    assert dirty.release.release_id != clean.release.release_id
    assert dirty.release.source_snapshot != clean.release.source_snapshot


def test_group_write_mode_keeps_the_canonical_clean_git_identity(source_checkout: Path) -> None:
    """Public source modes match Git and ZIP executable semantics across host umasks."""
    clean = capture_public_sources(source_checkout)
    (source_checkout / "README.md").chmod(0o664)
    assert capture_public_sources(source_checkout) == clean


@pytest.mark.parametrize("dirty", [False, True], ids=["clean", "dirty"])
def test_source_export_retains_identity_without_git_until_public_sources_change(
    source_checkout: Path, tmp_path: Path, *, dirty: bool
) -> None:
    """Rebuilt corresponding sources retain the ID only while exact source bytes match."""
    if dirty:
        (source_checkout / "README.md").write_bytes(b"Changed before build\n")
    captured = capture_public_sources(source_checkout)
    exported = tmp_path / "export"
    exported.mkdir()
    for source in captured.files:
        path = exported / source.path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source.contents)
        path.chmod(source.mode)
    (exported / ".fplinux-release.json").write_bytes(source_identity_bytes(captured))
    generated = exported / "site/dist/index.html"
    generated.parent.mkdir(parents=True)
    generated.write_bytes(b"generated website\n")
    assert not (exported / ".git").exists()
    restored = capture_public_sources(exported)
    assert restored.files == captured.files
    assert restored.release == captured.release
    (exported / "targets/first/target.toml").write_bytes(b'platform = "changed"\n')
    changed = current_release_context(exported)
    assert changed.release_id != captured.release.release_id
    assert changed.release_id == changed.source_snapshot[:12]
    assert changed.source_commit == captured.release.source_commit


def test_removing_git_alone_preserves_public_inventory(source_checkout: Path) -> None:
    """Filesystem inventory includes the same dotfiles, workflows and source modes."""
    captured = capture_public_sources(source_checkout)
    (source_checkout / ".fplinux-release.json").write_bytes(source_identity_bytes(captured))
    shutil.rmtree(source_checkout / ".git")
    assert capture_public_sources(source_checkout) == captured
