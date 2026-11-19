# SPDX-License-Identifier: GPL-2.0-only
"""Read entries and byte ranges from a Linux newc archive."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class NewcEntry:
    """One newc header and the location and bytes of its payload."""

    name: str
    mode: int
    uid: int
    gid: int
    inode: tuple[int, int, int]
    links: int
    device: tuple[int, int]
    offset: int
    data: bytes


def newc_entries(data: bytes) -> list[NewcEntry]:
    """Read the first archive through its trailer, including payload offsets."""
    entries: list[NewcEntry] = []
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset : offset + 110]
        if header[:6] != b"070701":
            message = "root filesystem is not a newc archive"
            raise ValueError(message)
        fields = [int(header[index : index + 8], 16) for index in range(6, 110, 8)]
        inode, mode, uid, gid, links, _mtime, size, major, minor, rmajor, rminor, namesize, _ = (
            fields
        )
        name_end = offset + 110 + namesize
        payload_start = (name_end + 3) & ~3
        payload_end = payload_start + size
        if not namesize or payload_end > len(data) or data[name_end - 1] != 0:
            message = "root filesystem archive is truncated"
            raise ValueError(message)
        name = data[offset + 110 : name_end - 1].decode("utf-8")
        if name == "TRAILER!!!":
            return entries
        entries.append(
            NewcEntry(
                name.removeprefix("./"),
                mode,
                uid,
                gid,
                (major, minor, inode),
                links,
                (rmajor, rminor),
                payload_start,
                data[payload_start:payload_end],
            )
        )
        offset = (payload_end + 3) & ~3
    message = "root filesystem archive has no trailer"
    raise ValueError(message)
