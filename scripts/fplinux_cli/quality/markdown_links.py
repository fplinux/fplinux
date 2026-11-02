# SPDX-License-Identifier: GPL-2.0-only
"""Validate repository-local Markdown links and anchors."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING
from urllib.parse import unquote

from fplinux_cli.common import ROOT, fail

if TYPE_CHECKING:
    from pathlib import Path

MARKDOWN_REFERENCE = re.compile(r"^\s*\[[^]]+\]:\s*(?:<([^>]+)>|(\S+))")
MARKDOWN_HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$")


def markdown_anchors(path: Path) -> set[str]:
    """Return GitHub-style anchors declared by one Markdown document."""
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    fenced = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = MARKDOWN_HEADING.fullmatch(line)
        if match is None:
            continue
        heading = re.sub(r"`([^`]*)`", r"\1", match.group(2)).lower()
        base = re.sub(r"[^\w\- ]", "", heading).replace(" ", "-")
        count = counts.get(base, 0)
        counts[base] = count + 1
        anchors.add(base if count == 0 else f"{base}-{count}")
    return anchors


def markdown_inline_destinations(line: str) -> list[str]:
    """Extract inline GFM destinations, including balanced parentheses."""
    destinations: list[str] = []
    position = 0
    while (opening := line.find("](", position)) >= 0:
        cursor = opening + 2
        while cursor < len(line) and line[cursor].isspace():
            cursor += 1
        if cursor >= len(line):
            break
        if line[cursor] == "<":
            closing = line.find(">", cursor + 1)
            if closing < 0:
                position = cursor + 1
                continue
            destinations.append(line[cursor + 1 : closing])
            position = closing + 1
            continue

        value: list[str] = []
        depth = 0
        while cursor < len(line):
            character = line[cursor]
            if character == "\\" and cursor + 1 < len(line):
                value.append(line[cursor + 1])
                cursor += 2
                continue
            if character == "(":
                depth += 1
            elif character == ")":
                if depth == 0:
                    break
                depth -= 1
            elif character.isspace() and depth == 0:
                break
            value.append(character)
            cursor += 1
        if value:
            destinations.append("".join(value))
        position = cursor + 1
    return destinations


def markdown_path_uses_symlink(path: Path, root: Path) -> bool:
    """Return whether a lexical path crosses a symlink inside the source tree."""
    current = path
    while current != root and root in current.parents:
        if current.is_symlink():
            return True
        current = current.parent
    return current.is_symlink()


def check_markdown_links(files: list[Path]) -> None:
    """Require every repository-local Markdown path and anchor to resolve."""
    anchor_cache: dict[Path, set[str]] = {}
    for source in files:
        fenced = False
        for number, raw_line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if raw_line.lstrip().startswith(("```", "~~~")):
                fenced = not fenced
                continue
            if fenced:
                continue
            line = re.sub(r"`[^`]*`", "", raw_line)
            destinations = markdown_inline_destinations(line)
            reference = MARKDOWN_REFERENCE.match(line)
            if reference is not None:
                reference_destination = reference.group(1) or reference.group(2)
                if reference_destination is None:
                    fail(
                        f"Markdown reference destination is invalid: "
                        f"{source.relative_to(ROOT)}:{number}"
                    )
                destinations.append(reference_destination)
            for raw_destination in destinations:
                destination = raw_destination.strip("<>")
                if re.match(r"^[a-z][a-z0-9+.-]*:", destination, re.IGNORECASE):
                    continue
                path_text, separator, fragment = destination.partition("#")
                candidate = source if not path_text else source.parent / unquote(path_text)
                root = ROOT.absolute()
                if markdown_path_uses_symlink(candidate, root):
                    fail(
                        f"Markdown link target is a symlink: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                target = candidate.resolve()
                try:
                    target.relative_to(ROOT.resolve())
                except ValueError:
                    fail(
                        f"Markdown link escapes the source tree: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                if not target.is_file():
                    fail(
                        f"Markdown link target is missing: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                if separator and target.suffix.lower() == ".md":
                    anchor = unquote(fragment).lower()
                    anchors = anchor_cache.setdefault(target, markdown_anchors(target))
                    if anchor not in anchors:
                        fail(
                            f"Markdown link anchor is missing: "
                            f"{source.relative_to(ROOT)}:{number}: {destination}"
                        )
