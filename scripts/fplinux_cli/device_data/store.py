# SPDX-License-Identifier: GPL-2.0-only
"""Locate and lock the host user's persistent phone data."""

from __future__ import annotations

import os
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.runtime.state import exclusive_lock, private_directory

if TYPE_CHECKING:
    from collections.abc import Iterator


def phone_store_directory(target: str, *, root: Path | None = None) -> Path:
    """Resolve one phone's store independently of a checkout or release archive."""
    if root is not None:
        return root
    if sys.platform == "win32":
        local_app_data = os.environ.get("LOCALAPPDATA")
        if not local_app_data:
            fail("LOCALAPPDATA is required to locate the private phone data store.")
        return Path(local_app_data) / "FPLinux" / "phones" / target
    data_home = os.environ.get("XDG_DATA_HOME")
    root_data = Path(data_home) if data_home else Path.home() / ".local" / "share"
    return root_data / "fplinux" / "phones" / target


@contextmanager
def locked_phone_store(target: str, *, root: Path | None = None) -> Iterator[Path]:
    """Serialize backup publication, preparation and snapshots of one phone store."""
    directory = private_directory(phone_store_directory(target, root=root))
    with exclusive_lock(
        directory / ".store.lock",
        message="This phone's data is being updated. Retry after the current operation finishes.",
    ):
        yield directory
