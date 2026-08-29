# SPDX-License-Identifier: GPL-2.0-only
"""Classify project sources by their existing pinned formatter."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


@dataclass(frozen=True)
class SourceFormats:
    """Repository-relative source paths grouped by formatter contract."""

    python: tuple[str, ...]
    markdown: tuple[str, ...]
    json: tuple[str, ...]
    toml: tuple[str, ...]
    posix_shell: tuple[str, ...]
    bash: tuple[str, ...]
    c: tuple[str, ...]

    def supported(self) -> frozenset[str]:
        """Return every source path owned by one formatter."""
        return frozenset(
            (
                *self.python,
                *self.markdown,
                *self.json,
                *self.toml,
                *self.posix_shell,
                *self.bash,
                *self.c,
            )
        )


def shell_dialect(raw_first_line: bytes) -> Literal["posix", "bash"] | None:
    """Return the formatter dialect selected by one exact source shebang."""
    try:
        first_line = raw_first_line.decode().strip()
    except UnicodeDecodeError:
        return None
    if first_line == "#!/usr/bin/env bash":
        return "bash"
    if first_line in {"#!/bin/sh", "#!/usr/bin/env sh", "#!/sbin/openrc-run"}:
        return "posix"
    return None


def classify_source_formats(files: Sequence[Path], *, root: Path) -> SourceFormats:
    """Apply the quality gate's formatter routing to regular source paths."""
    python: list[str] = []
    markdown: list[str] = []
    json: list[str] = []
    toml: list[str] = []
    posix_shell: list[str] = []
    bash: list[str] = []
    c: list[str] = []

    for path in files:
        relative = path.relative_to(root).as_posix()
        if path.suffix == ".py":
            python.append(relative)
        elif path.suffix == ".md":
            markdown.append(relative)
        elif path.suffix in {".json", ".jsonc"} and path.name != "package-lock.json":
            json.append(relative)
        elif path.suffix == ".toml":
            toml.append(relative)
        elif path.suffix in {".c", ".h"}:
            c.append(relative)

        if path.suffix not in {"", ".initd", ".sh"}:
            continue
        with path.open("rb") as stream:
            raw_first_line = stream.readline()
        dialect = shell_dialect(raw_first_line)
        if dialect == "bash":
            bash.append(relative)
        elif dialect == "posix":
            posix_shell.append(relative)

    return SourceFormats(
        tuple(sorted(python)),
        tuple(sorted(markdown)),
        tuple(sorted(json)),
        tuple(sorted(toml)),
        tuple(sorted(posix_shell)),
        tuple(sorted(bash)),
        tuple(sorted(c)),
    )
