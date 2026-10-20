# SPDX-License-Identifier: GPL-2.0-only
"""Extract the locked Alpine rootfs while preserving mapped logical ownership."""

from __future__ import annotations

import argparse
import tarfile
from pathlib import Path

from fplinux_cli.common import alpine_tar_filter, fail


def _preserve_metadata(member: tarfile.TarInfo, target: str) -> tarfile.TarInfo | None:
    filtered = alpine_tar_filter(member, target, on_error=fail)
    if filtered is None:
        return None
    return filtered.replace(
        uid=member.uid,
        gid=member.gid,
        mode=member.mode,
    )


def extract_rootfs(archive: Path, destination: Path) -> None:
    """Extract only data-safe members, retaining the archive's owner and mode metadata."""
    with tarfile.open(archive, "r:gz") as bundle:
        bundle.extractall(  # noqa: S202 -- each member passes Alpine's data filter.
            destination, filter=_preserve_metadata, numeric_owner=True
        )


def main() -> None:
    """Run extraction inside the same mapped namespace as environment installation."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("destination", type=Path)
    arguments = parser.parse_args()
    extract_rootfs(arguments.archive, arguments.destination)


if __name__ == "__main__":
    main()
