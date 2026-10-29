# SPDX-License-Identifier: GPL-2.0-only
"""The one shared/exclusive lock for FPLinux's cache."""

from __future__ import annotations

import errno
import fcntl
import os
import sys
import time
from contextlib import contextmanager
from datetime import UTC, datetime
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


_LOCK_FILENAME = ".fplinux-cache.lock"


def _owner_text(command: str, target: str | None, profile: str | None) -> str:
    """Render the short status shown to a command that has to wait."""
    started = datetime.now(UTC).isoformat(timespec="seconds")
    return (
        f"command={command} target={target or '-'} profile={profile or '-'} "
        f"pid={os.getpid()} started={started}"
    )


def _read_owner(descriptor: int) -> str:
    """Return the current holder's status, if it left one."""
    return os.pread(descriptor, 4096, 0).decode(errors="replace").strip() or "owner unavailable"


def _write_owner(descriptor: int, owner: str) -> None:
    """Publish the current holder while its flock is held."""
    os.ftruncate(descriptor, 0)
    os.write(descriptor, f"{owner}\n".encode())


@contextmanager
def cache_lock(  # noqa: PLR0913 -- ownership and wait policy are distinct lock inputs.
    cache_root: Path,
    *,
    exclusive: bool,
    command: str,
    target: str | None,
    profile: str | None = None,
    timeout: float | None = None,
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
    acquired = False
    try:
        try:
            fcntl.flock(descriptor, operation | fcntl.LOCK_NB)
            acquired = True
        except OSError as error:
            if error.errno not in {errno.EACCES, errno.EAGAIN}:
                raise
            owner = _read_owner(descriptor)
            if timeout is None or timeout > 0:
                print(
                    f"fplinux: cache is held by {owner}; waiting...", file=sys.stderr, flush=True
                )
            if timeout is None:
                fcntl.flock(descriptor, operation)
                acquired = True
                waited = True
            else:
                deadline = time.monotonic() + timeout
                while not acquired:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        print(
                            f"fplinux: cache is held by {_read_owner(descriptor)}; "
                            "lock wait timed out",
                            file=sys.stderr,
                            flush=True,
                        )
                        raise SystemExit(75) from None
                    time.sleep(min(0.1, remaining))
                    try:
                        fcntl.flock(descriptor, operation | fcntl.LOCK_NB)
                        acquired = True
                        waited = True
                    except OSError as retry_error:
                        if retry_error.errno not in {errno.EACCES, errno.EAGAIN}:
                            raise

        _write_owner(descriptor, _owner_text(command, target, profile))
        if not exclusive:
            # ``run`` and ``console`` replace this process with the runner.
            # Keep their shared flock alive until that process exits.
            os.set_inheritable(descriptor, True)  # noqa: FBT003 -- positional-only API.
        if waited:
            print("fplinux: cache released; continuing.", file=sys.stderr, flush=True)
        yield
    finally:
        if acquired:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)
