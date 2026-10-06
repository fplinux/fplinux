# SPDX-License-Identifier: GPL-2.0-only
"""Materialize the bounded set of Linux integration destinations."""

from __future__ import annotations

import re
import stat
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli.build.sources import append_steps, apply_patches, copy_steps
from fplinux_cli.common import fail
from fplinux_cli.manifests.values import relative_value
from fplinux_cli.workspace.capture import WorkspaceFile

if TYPE_CHECKING:
    from collections.abc import Iterator, Sequence
    from pathlib import Path


@dataclass(frozen=True)
class LinuxInput:
    """One existing build integration operation, in its declared order."""

    operation: str
    identity: str
    destination: str
    source: Path

    def destinations(self) -> tuple[str, ...]:
        """Resolve patch paths or the explicit copy/append destination."""
        if self.operation.endswith("patch"):
            return patch_destinations(self.source)
        return (self.destination,)


def patch_destinations(path: Path, *, include_deleted: bool = True) -> tuple[str, ...]:
    """Read paths from a text patch, accounting for complete hunk boundaries."""
    result: list[str] = []
    old_remaining = new_remaining = 0
    old_path = ""
    hunk = re.compile(r"^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")
    for line in path.read_text().splitlines():
        if old_remaining or new_remaining:
            if line.startswith("\\"):
                continue
            marker = line[:1] or " "
            if marker in {" ", "-"}:
                old_remaining -= 1
            if marker in {" ", "+"}:
                new_remaining -= 1
            if marker not in {" ", "-", "+"} or min(old_remaining, new_remaining) < 0:
                fail(f"malformed Linux patch hunk: {path}")
            continue
        match = hunk.match(line)
        if match:
            old_remaining = int(match.group(1) or "1")
            new_remaining = int(match.group(2) or "1")
        elif line.startswith("--- "):
            old_path = line[4:].split("\t", 1)[0]
        elif line.startswith("+++ ") and old_path:
            new_path = line[4:].split("\t", 1)[0]
            if new_path == "/dev/null" and not include_deleted:
                old_path = ""
                continue
            name = old_path if new_path == "/dev/null" else new_path
            prefix, separator, destination = name.partition("/")
            if not separator or prefix in {"", ".."}:
                fail(f"Linux patch destination has no relative -p1 prefix: {path}")
            result.append(relative_value(destination, "Linux patch destination"))
            old_path = ""
    if old_remaining or new_remaining:
        fail(f"incomplete Linux patch hunk: {path}")
    return tuple(dict.fromkeys(result))


def write_files(root: Path, files: dict[str, WorkspaceFile]) -> None:
    """Materialize a small source projection, not an additional Linux checkout."""
    for name, source in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source.contents)
        path.chmod(source.mode)


def file_contents(root: Path, names: Sequence[str]) -> dict[str, WorkspaceFile]:
    """Capture existing files, preserving absence for additions and deletions."""
    return {
        name: WorkspaceFile(
            name, (root / name).read_bytes(), stat.S_IMODE((root / name).stat().st_mode)
        )
        for name in names
        if (root / name).is_file()
    }


def project_changes(
    base: dict[str, WorkspaceFile],
    inputs: Sequence[LinuxInput],
    root: Path,
) -> Iterator[tuple[LinuxInput, dict[str, WorkspaceFile], dict[str, WorkspaceFile]]]:
    """Replay the same patch/copy/append sequence used by the public build."""
    write_files(root, base)
    for step in inputs:
        names = step.destinations()
        before = file_contents(root, names)
        if step.operation.endswith("patch"):
            apply_patches(root, [step.source])
        elif step.operation.endswith("copy"):
            copy_steps(root, [(step.source, step.destination)])
        else:
            append_steps(root, [(step.source, step.destination)])
        yield step, before, file_contents(root, names)
