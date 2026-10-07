# SPDX-License-Identifier: GPL-2.0-only
"""Classify project sources by their canonical text contract."""

from __future__ import annotations

from dataclasses import dataclass, fields, replace
from pathlib import PurePosixPath
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
    javascript: tuple[str, ...]
    posix_shell_fragments: tuple[str, ...]
    yaml: tuple[str, ...] = ()
    devicetree: tuple[str, ...] = ()
    text: tuple[str, ...] = ()

    def supported(self) -> frozenset[str]:
        """Return every source path owned by one formatter."""
        return frozenset(path for field in fields(self) for path in getattr(self, field.name))

    def select(self, selected: frozenset[str]) -> SourceFormats:
        """Retain the exact selected paths without reclassifying their contents."""
        return replace(
            self,
            **{
                field.name: tuple(path for path in getattr(self, field.name) if path in selected)
                for field in fields(self)
            },
        )


def source_format_kind(relative: str) -> str | None:  # noqa: PLR0911 -- distinct filename contracts.
    """Return the filename-owned contract, including supported source templates."""
    path = PurePosixPath(relative.removesuffix(".in"))
    suffix = path.suffix
    if path.parts[:1] == ("site",) and suffix in {
        ".astro",
        ".css",
        ".js",
        ".json",
        ".jsonc",
        ".md",
        ".mdx",
        ".mjs",
        ".ts",
        ".yaml",
        ".yml",
    }:
        return None
    if suffix == ".py":
        return "python"
    if suffix == ".md":
        return "markdown"
    if suffix in {".json", ".jsonc"} and path.name != "package-lock.json":
        return "json"
    if suffix == ".toml":
        return "toml"
    if suffix in {".yaml", ".yml"}:
        return "yaml"
    if suffix in {".c", ".h"}:
        return "c"
    if suffix in {".dts", ".dtsi"}:
        return "devicetree"
    if is_javascript_source(relative):
        return "javascript"
    if (
        path.name in {"APKBUILD", ".editorconfig", "Makefile", "Kconfig", "Config.in"}
        or suffix in {".Makefile", ".Kconfig", ".mk", ".ini", ".fragment", ".s", ".S"}
        or (relative.endswith(".txt.in"))
    ):
        return "text"
    return None


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


def is_javascript_source(relative: str) -> bool:
    """Identify the project-owned JavaScript configuration."""
    return relative in {
        "commitlint.config.mjs",
        "scripts/fplinux_cli/quality/formatting/canonical_json_tree.mjs",
    }


def is_posix_shell_fragment(relative: str) -> bool:
    """Identify sourced configuration files with a declared POSIX dialect."""
    return relative == "alpine/abuild.conf"


def classify_source_formats(files: Sequence[Path], *, root: Path) -> SourceFormats:
    """Apply the quality gate's formatter routing to regular source paths."""
    groups: dict[str, list[str]] = {field.name: [] for field in fields(SourceFormats)}

    for path in files:
        relative = path.relative_to(root).as_posix()
        kind = source_format_kind(relative)
        if kind is not None:
            groups[kind].append(relative)
        if is_posix_shell_fragment(relative):
            groups["posix_shell_fragments"].append(relative)
            continue
        if path.suffix not in {"", ".initd", ".sh", ".bashrc"}:
            continue
        with path.open("rb") as stream:
            raw_first_line = stream.readline()
        dialect = shell_dialect(raw_first_line)
        if dialect == "bash":
            groups["bash"].append(relative)
        elif dialect == "posix":
            groups["posix_shell"].append(relative)

    return SourceFormats(**{name: tuple(sorted(paths)) for name, paths in groups.items()})
