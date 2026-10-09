# SPDX-License-Identifier: GPL-2.0-only
"""Create deterministic supplemental initramfs archives from admitted device data."""

from __future__ import annotations

import stat
from pathlib import PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

from .inputs import firmware_storage

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

    from .inputs import FirmwareInput


def create_device_data_initramfs(
    groups: Mapping[str, Sequence[FirmwareInput]],
) -> bytes:
    """Package optional runtime firmware as newc."""
    files: dict[str, bytes] = {}
    for group in ("bluetooth", "fm-radio", "audio-profile", "wcn-firmware"):
        for firmware in groups.get(group, ()):
            stored_name, contents = firmware_storage(firmware)
            path = f"lib/firmware/{stored_name}"
            if path in files:
                fail(f"firmware destination already exists: {path}")
            files[path] = contents

    directories: set[str] = set()
    for path in files:
        directories.update(
            parent.as_posix() for parent in PurePosixPath(path).parents if parent.parts
        )
    if directories.intersection(files):
        fail("firmware destination conflicts with a parent directory")

    archive = bytearray()
    for inode, path in enumerate(sorted(directories | files.keys()), start=1):
        if path in directories:
            mode = stat.S_IFDIR | 0o755
            contents = b""
        else:
            mode = stat.S_IFREG | 0o600
            contents = files[path]
        _append_entry(archive, path, contents, inode=inode, mode=mode)
    _append_entry(archive, "TRAILER!!!", b"", inode=0, mode=0)
    archive.extend(b"\0" * (-len(archive) % 512))
    return bytes(archive)


def _append_entry(
    archive: bytearray,
    path: str,
    contents: bytes,
    *,
    inode: int,
    mode: int,
) -> None:
    """Append a newc entry with zero ownership and time and four-byte alignment."""
    name = path.encode("utf-8") + b"\0"
    fields = (
        inode,
        mode,
        0,  # uid
        0,  # gid
        2 if stat.S_ISDIR(mode) else 1,
        0,  # mtime
        len(contents),
        0,  # device major
        0,  # device minor
        0,  # special device major
        0,  # special device minor
        len(name),
        0,  # checksum for the newc format
    )
    archive.extend(("070701" + "".join(f"{value:08x}" for value in fields)).encode("ascii"))
    archive.extend(name)
    archive.extend(b"\0" * (-len(archive) % 4))
    archive.extend(contents)
    archive.extend(b"\0" * (-len(archive) % 4))
