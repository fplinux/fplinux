# SPDX-License-Identifier: GPL-2.0-only
"""Validate repository directory names."""

from __future__ import annotations

import re
from pathlib import PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

if TYPE_CHECKING:
    from collections.abc import Iterable


def validate_directory_names(inventory: Iterable[str]) -> None:
    """Require snake_case packages and kebab-case resource directories."""
    directories: set[PurePosixPath] = set()
    packages: set[PurePosixPath] = set()
    for relative in inventory:
        path = PurePosixPath(relative)
        directories.update(parent for parent in path.parents if parent != PurePosixPath("."))
        if path.name == "__init__.py":
            packages.add(path.parent)
    for directory in sorted(directories):
        if directory.as_posix() in {"LICENSES", ".github/ISSUE_TEMPLATE"}:
            continue
        parts = directory.parts
        if directory.name == "Documentation" and (
            (
                len(parts) == 5
                and parts[0] in {"platforms", "targets"}
                and parts[2:4] == ("linux", "overlay")
            )
            or (
                len(parts) == 6
                and parts[0] == "platforms"
                and parts[2:5] == ("target-template", "linux", "overlay")
            )
        ):
            continue
        if directory in packages:
            if re.fullmatch(r"[a-z_][a-z0-9_]*", directory.name) is None:
                fail(f"Python package directory must use snake_case: {directory}")
        elif re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", directory.name.removeprefix(".")) is None:
            fail(f"source directory must use kebab-case: {directory}")
