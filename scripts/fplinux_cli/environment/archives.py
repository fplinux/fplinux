# SPDX-License-Identifier: GPL-2.0-only
"""Extract verified tool archives using the host's tar."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def extract_archive(
    archive: Path, destination: Path, *members: str, strip_components: int = 0
) -> None:
    """Extract the selected archive members into an installation directory."""
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "tar",
            "--warning=no-unknown-keyword",
            "--extract",
            "--file",
            archive,
            "--directory",
            destination,
            f"--strip-components={strip_components}",
            *members,
        ],
        check=True,
    )
