# SPDX-License-Identifier: GPL-2.0-only
"""Inspect member boundaries in produced RAM boot archives."""

from __future__ import annotations

from fplinux_cli.artifacts.newc import newc_entries


def ram_bootstrap_xz_offset(archive: bytes) -> int:
    """Find the XZ stream after the last raw newc payload."""
    last_entry = newc_entries(archive)[-1]
    payload_end = last_entry.offset + len(last_entry.data)
    return archive.index(b"\xfd7zXZ\x00", payload_end)
