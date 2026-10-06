# SPDX-License-Identifier: GPL-2.0-only
"""Select current Git source inventory for quality workspaces."""

from __future__ import annotations

import os
import stat
import subprocess
from pathlib import PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail

from .capture import WorkspaceSnapshot, snapshot_from_inventory

if TYPE_CHECKING:
    from pathlib import Path

_GIT_INVENTORY_TIMEOUT = 60


def quality_files(*, enforce_source_policy: bool) -> list[tuple[str, Path]]:
    """Return tracked and non-ignored untracked files used by quality checks."""
    command = [
        "git",
        "-C",
        str(ROOT),
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            check=False,
            timeout=_GIT_INVENTORY_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Git source inventory timed out after {_GIT_INVENTORY_TIMEOUT}s")
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


def quality_workspace_snapshot(*, enforce_source_policy: bool) -> WorkspaceSnapshot:
    """Read the complete quality closure before deciding whether staging is needed."""
    return snapshot_from_inventory(
        lambda: quality_files(enforce_source_policy=enforce_source_policy)
    )
