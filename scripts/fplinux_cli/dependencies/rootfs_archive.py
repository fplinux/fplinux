# SPDX-License-Identifier: GPL-2.0-only
"""Validate rootfs archive paths before a mapped extraction can change files."""

from __future__ import annotations

import tarfile
from pathlib import PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

if TYPE_CHECKING:
    from pathlib import Path


def _member_name(value: str) -> str:
    name = value.removeprefix("./")
    if name == ".":
        return name
    path = PurePosixPath(name)
    if (
        not name
        or "\x00" in name
        or path.is_absolute()
        or path.as_posix() != name
        or any(part in {".", ".."} for part in path.parts)
    ):
        fail(f"dependency rootfs archive path is invalid: {value}")
    return name


def _require_rooted_symlink(name: str, target: str) -> None:
    if not target or "\x00" in target:
        fail(f"dependency rootfs archive link is invalid: {name}")
    parts = [] if target.startswith("/") else list(PurePosixPath(name).parts[:-1])
    for part in PurePosixPath(target).parts:
        if part in {"/", "."}:
            continue
        if part == "..":
            if not parts:
                fail(f"dependency rootfs archive link escapes the root: {name}")
            parts.pop()
        else:
            parts.append(part)


def require_rootfs_archive(archive: Path) -> None:
    """Reject traversal and link-parent extraction while retaining native rootfs metadata."""
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            members: dict[str, tarfile.TarInfo] = {}
            for member in bundle:
                name = _member_name(member.name)
                if name in members:
                    fail(f"dependency rootfs archive contains a duplicate path: {name}")
                if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                    fail(f"dependency rootfs archive entry type is invalid: {name}")
                if name == "." and not member.isdir():
                    fail("dependency rootfs archive root is not a directory")
                if member.uid < 0 or member.gid < 0 or member.mode < 0 or member.mode > 0o7777:
                    fail(f"dependency rootfs archive ownership or mode is invalid: {name}")
                if member.issym():
                    _require_rooted_symlink(name, member.linkname)
                members[name] = member
            if not members:
                fail("dependency rootfs archive is empty")
            for name, member in members.items():
                for parent in PurePosixPath(name).parents:
                    parent_name = parent.as_posix()
                    if parent_name != "." and (
                        parent_name not in members or not members[parent_name].isdir()
                    ):
                        fail(f"dependency rootfs archive parent is not a directory: {name}")
                if member.islnk():
                    target = _member_name(member.linkname)
                    visited = {name}
                    while target in members and members[target].islnk() and target not in visited:
                        visited.add(target)
                        target = _member_name(members[target].linkname)
                    if target not in members or not members[target].isfile():
                        fail(f"dependency rootfs archive hard link is invalid: {name}")
    except (OSError, tarfile.TarError) as error:
        fail(f"dependency rootfs archive cannot be verified: {error}")
