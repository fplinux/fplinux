# SPDX-License-Identifier: GPL-2.0-only
"""Controlled installed filesystem inputs for image identities."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def installed_tree(root: Path) -> Path:
    """Prepare actual tool bytes, a shared library and an operational state marker."""
    compiler = root / "usr/bin/cc"
    compiler.parent.mkdir(parents=True)
    compiler.write_bytes(b"compiler one\n")
    compiler.chmod(0o755)
    library = root / "usr/lib/libc.so"
    library.parent.mkdir()
    library.write_bytes(b"shared library one\n")
    library.chmod(0o644)
    (root / "usr/bin/compiler").symlink_to("cc")
    state = root / "etc/fplinux-image-state"
    state.parent.mkdir()
    state.write_bytes(b"first recipe\nfirst generation\nfirst content\n")
    return compiler
