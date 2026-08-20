# SPDX-License-Identifier: GPL-2.0-only
"""The one shared/exclusive lock for FPLinux's cache."""

from __future__ import annotations

import errno
import fcntl
import os
import sys
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


_LOCK_FILENAME = ".fplinux-cache.lock"


def _owner_text(command: str, target: str | None) -> str:
    """Render the short status shown to a command that has to wait."""
    started = datetime.now(UTC).isoformat(timespec="seconds")
    return f"command={command} target={target or '-'} pid={os.getpid()} started={started}"


def _read_owner(descriptor: int) -> str:
    """Return the current holder's status, if it left one."""
    return os.pread(descriptor, 4096, 0).decode(errors="replace").strip() or "owner unavailable"


def _write_owner(descriptor: int, owner: str) -> None:
    """Publish the current holder while its flock is held."""
    os.ftruncate(descriptor, 0)
    os.write(descriptor, f"{owner}\n".encode())


@contextmanager
def cache_lock(
    cache_root: Path,
    *,
    exclusive: bool,
    command: str,
    target: str | None,
) -> Iterator[None]:
    """Hold the global cache flock and report one blocking owner, if any."""
    cache_root.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(
        cache_root / _LOCK_FILENAME,
        os.O_RDWR | os.O_CREAT | os.O_CLOEXEC,
        0o600,
    )
    operation = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
    waited = False
    try:
        try:
            fcntl.flock(descriptor, operation | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno not in {errno.EACCES, errno.EAGAIN}:
                raise
            print(
                f"fplinux: cache is held by {_read_owner(descriptor)}; waiting...",
                file=sys.stderr,
                flush=True,
            )
            fcntl.flock(descriptor, operation)
            waited = True

        _write_owner(descriptor, _owner_text(command, target))
        if not exclusive:
            # ``run`` and ``console`` replace this process with the runner.
            # Keep their shared flock alive until that process exits.
            os.set_inheritable(descriptor, True)  # noqa: FBT003 -- positional-only API.
        if waited:
            print("fplinux: cache released; continuing.", file=sys.stderr, flush=True)
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)
