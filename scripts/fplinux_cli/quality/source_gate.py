# SPDX-License-Identifier: GPL-2.0-only
"""Validate repository directory names and the build-image source policy."""

from __future__ import annotations

import os
import re
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli import common
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
        if directory in packages:
            if re.fullmatch(r"[a-z_][a-z0-9_]*", directory.name) is None:
                fail(f"Python package directory must use snake_case: {directory}")
        elif re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", directory.name.removeprefix(".")) is None:
            fail(f"source directory must use kebab-case: {directory}")


def containerfile_uses_locked_base(contents: str) -> bool:
    """Require an initial BASE_IMAGE argument and one exact lock-provided FROM."""
    instructions = [
        line.strip()
        for line in contents.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    return from_instructions == ["FROM ${BASE_IMAGE}"] and instructions[0] == "ARG BASE_IMAGE"


def validate_source_policy() -> None:
    """Enforce the single-image source policy."""
    ignored = {".cache", ".git", "node_modules"}
    containerfiles: list[Path] = []
    # Prune the generated trees before descending: they hold millions of
    # cached build paths that a plain rglob would stat one by one.
    for directory, subdirectories, names in os.walk(common.ROOT):
        subdirectories[:] = [name for name in subdirectories if name not in ignored]
        if Path(directory) == common.ROOT / "site":
            subdirectories[:] = [name for name in subdirectories if name not in {".astro", "dist"}]
        for name in names:
            if name != "Containerfile":
                continue
            relative = (Path(directory) / name).relative_to(common.ROOT)
            containerfiles.append(relative)
    containerfiles.sort()
    if containerfiles != [Path("Containerfile")]:
        fail("source must contain exactly one root Containerfile")
    if not containerfile_uses_locked_base((common.ROOT / containerfiles[0]).read_text()):
        fail("Containerfile must use one lock-provided FROM ${BASE_IMAGE}")
