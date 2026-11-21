# SPDX-License-Identifier: GPL-2.0-only
"""Test peer for the private audiod packet layouts."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from socket import socket

SYNC = b"\x07"
RADIO_READY = b"\x03"
_CLASSES = {"MEDIA": 1, "ALARM": 2}
_ACTIVITY = b"\x0b"
_ACTIVITY_STATES = ("none", "paused", "held", "unavailable", "playing")
_STATUS_FIELDS = (
    "media",
    "alarm",
    "rate",
    "output",
    "applied",
    "speaker",
    "headphones",
    "jack",
    "plugged",
    "fm",
    "fm_active",
    "clock",
    "foreground",
    "pcm",
    "clipped",
    "error",
)


def set_volume(kind: str, level: int) -> bytes:
    return bytes((1, _CLASSES[kind], level))


def volume_applied(kind: str, level: int) -> bytes:
    return bytes((8, _CLASSES[kind], level))


def set_output(route: int) -> bytes:
    return bytes((2, route))


def output_applied(route: int) -> bytes:
    return bytes((9, route))


def app(pid: int, uid: int, *, background: bool = False) -> bytes:
    return struct.pack("<BIIB", 3, pid, uid, background)


def clock(pid: int, uid: int) -> bytes:
    return struct.pack("<BII", 4, pid, uid)


def gone(pid: int) -> bytes:
    return struct.pack("<BI", 5, pid)


def eligible(pid: int, *, enabled: bool) -> bytes:
    return struct.pack("<BIB", 6, pid, enabled)


def is_activity(packet: bytes) -> bool:
    return packet.startswith(_ACTIVITY)


def activity(packet: bytes) -> tuple[int, int, str]:
    kind, pid, uid, state = struct.unpack("<BIIB", packet)
    assert kind == 11 and pid > 0 and uid < 2**32 - 1
    return pid, uid, _ACTIVITY_STATES[state]


def status(client: socket) -> dict[str, int]:
    kind, *fields = struct.unpack("<BBBI11BQI", client.recv(512))
    assert kind == 1
    assert client.recv(1) == b""
    return dict(zip(_STATUS_FIELDS, fields, strict=True))


def radio_play(frequency: int) -> bytes:
    return struct.pack("<BH", 2, frequency)
