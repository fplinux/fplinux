# SPDX-License-Identifier: GPL-2.0-only
"""Protect private host directories and coordinate their writers."""

from __future__ import annotations

import errno
import os
import sys
from contextlib import contextmanager
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


def private_directory(path: Path) -> Path:
    """Protect personal files and connection keys on the selected host."""
    if os.name == "nt":
        from .windows_host import WindowsHost  # noqa: PLC0415

        return WindowsHost().runtime_root(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        fail(f"Private data directory is invalid: {path}")
    if path.stat().st_uid != os.getuid():
        fail(f"Private data directory belongs to another user: {path}")
    path.chmod(0o700)
    return path


@contextmanager
def exclusive_lock(path: Path, *, message: str) -> Iterator[None]:
    """Hold one stable lock file until its protected operation finishes."""
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            if sys.platform == "win32":
                import msvcrt  # noqa: PLC0415 -- Windows-only process locking.

                msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl  # noqa: PLC0415 -- Linux-only process locking.

                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in {errno.EACCES, errno.EAGAIN, errno.EDEADLK}:
                raise
            fail(message)
        yield
    finally:
        # Reusing the inode prevents a competing writer from locking another file.
        os.close(descriptor)
