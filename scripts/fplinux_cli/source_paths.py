# SPDX-License-Identifier: GPL-2.0-only
"""Keep managed data and local private material outside source admission."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.data_paths import DATA_DIR_NAME, PRIVATE_DIR_NAME

if TYPE_CHECKING:
    from pathlib import Path


def source_path_excluded(path: Path, *, source_root: Path, selected_data_root: Path) -> bool:
    """Reject reserved trees before reading metadata or descending into them."""
    relative = path.relative_to(source_root)
    if any(part in {DATA_DIR_NAME, PRIVATE_DIR_NAME} for part in relative.parts):
        return True
    return (
        selected_data_root != source_root
        and selected_data_root.is_relative_to(source_root)
        and path.is_relative_to(selected_data_root)
    )


def git_source_excludes(*, source_root: Path, selected_data_root: Path) -> tuple[str, ...]:
    """Prune untracked working trees while Git inventories the checkout."""
    patterns = [DATA_DIR_NAME, PRIVATE_DIR_NAME]
    if selected_data_root != source_root and selected_data_root.is_relative_to(source_root):
        relative = selected_data_root.relative_to(source_root).as_posix()
        literal = "".join(
            "\\" + character if character in "\\*?[] " else character for character in relative
        )
        patterns.append("/" + literal)
    return tuple(f"--exclude={pattern}" for pattern in patterns)


def validate_checkout_data_root(source_root: Path, selected_data_root: Path) -> None:
    """Reject checkout placements that would manage source or private material."""
    source_root = source_root.resolve()
    selected_data_root = selected_data_root.resolve()
    if source_root.is_relative_to(selected_data_root):
        fail(f"managed data root must not contain the source checkout: {selected_data_root}")
    private = source_root / PRIVATE_DIR_NAME
    if selected_data_root.is_relative_to(private) or private.is_relative_to(selected_data_root):
        fail(f"managed data root must not overlap local private material: {selected_data_root}")
    if selected_data_root.is_relative_to(source_root / "site"):
        fail(f"managed data root must not be inside the website source tree: {selected_data_root}")
    if not selected_data_root.is_relative_to(source_root) or not (source_root / ".git").exists():
        return
    try:
        result = subprocess.run(
            ["git", "-C", str(source_root), "ls-files", "-z", "--cached"],
            capture_output=True,
            check=False,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        fail("managed data placement Git inventory timed out after 60s")
    if result.returncode:
        detail = result.stderr.decode(errors="replace").strip()
        fail(f"cannot validate managed data placement: {detail or 'git ls-files failed'}")
    for encoded in filter(None, result.stdout.split(b"\0")):
        tracked = source_root / os.fsdecode(encoded)
        if tracked.is_relative_to(selected_data_root) or selected_data_root.is_relative_to(
            tracked
        ):
            fail(f"managed data root overlaps tracked source: {selected_data_root}")
