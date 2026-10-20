# SPDX-License-Identifier: GPL-2.0-only
"""Identify installed content consumed by the build environment."""

from __future__ import annotations

import hashlib
import json
import stat
from pathlib import Path


def image_content_records(root: Path) -> list[dict[str, object]]:
    """Collect installed bytes, paths, owners, modes and links without timestamps."""
    records: list[dict[str, object]] = []
    # Runtime mounts and scratch directories do not supply installed build tools.
    for directory in ("bin", "etc", "home", "lib", "opt", "root", "sbin", "usr", "var"):
        base = root / directory
        if not base.exists():
            continue
        paths = [base, *sorted(base.rglob("*"))] if not base.is_symlink() else [base]
        for path in paths:
            relative = path.relative_to(root).as_posix()
            # Kern supplies these per invocation; they are not installed tool inputs.
            if relative in {
                "etc/fplinux-environment-state",
                "etc/hostname",
                "etc/hosts",
                "etc/resolv.conf",
            }:
                continue
            metadata = path.lstat()
            record: dict[str, object] = {
                "path": relative,
                "mode": stat.S_IMODE(metadata.st_mode),
                "uid": metadata.st_uid,
                "gid": metadata.st_gid,
            }
            if stat.S_ISLNK(metadata.st_mode):
                record["type"] = "symlink"
                record["target"] = path.readlink().as_posix()
            elif stat.S_ISDIR(metadata.st_mode):
                record["type"] = "directory"
            elif stat.S_ISREG(metadata.st_mode):
                record["type"] = "file"
                with path.open("rb") as source:
                    record["sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
            else:
                raise ValueError(f"unsupported installed image entry: {relative}")
            records.append(record)
    return records


def image_content_digest(root: Path) -> str:
    """Hash installed bytes, paths, owners, modes and links without filesystem timestamps."""
    records = image_content_records(root)
    encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def main() -> None:
    """Print the installed content identity in the current logical UID mapping."""
    print(image_content_digest(Path("/")))


if __name__ == "__main__":
    main()
