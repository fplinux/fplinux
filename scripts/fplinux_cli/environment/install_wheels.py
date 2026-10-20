# SPDX-License-Identifier: GPL-2.0-only
"""Install the exact local Python wheels declared for the build environment."""

from __future__ import annotations

import argparse
import hashlib
import tomllib
import zipfile
from pathlib import Path


def install_wheels(lock: Path, inputs: Path, destination: Path) -> None:
    """Verify local wheels before extracting their declared bytes."""
    with lock.open("rb") as stream:
        declarations = tomllib.load(stream)
    destination.mkdir(parents=True, exist_ok=True)
    for record in declarations["input"]:
        cached_path = Path(record["destination"])
        if record["purpose"] != "environment-source" or cached_path.suffix != ".whl":
            continue
        archive = inputs / cached_path
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != record["sha256"]:
            raise SystemExit(f"wheel SHA-256 mismatch: {record['key']}")
        if archive.stat().st_size != record["bytes"]:
            raise SystemExit(f"wheel size mismatch: {record['key']}")
        with zipfile.ZipFile(archive) as wheel:
            wheel.extractall(destination)


def main() -> None:
    """Install quality wheels from the verified local inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    install_wheels(args.lock, args.inputs, args.destination)


if __name__ == "__main__":
    main()
