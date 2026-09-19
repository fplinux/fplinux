# SPDX-License-Identifier: GPL-2.0-only
"""Independent file records for synthetic build-manifest inputs."""

from __future__ import annotations

import hashlib
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def file_record(path: Path) -> dict[str, int | str]:
    """Describe fixture bytes without using the production manifest writer."""
    return {
        "mode": path.stat().st_mode & 0o777,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "size": path.stat().st_size,
    }
