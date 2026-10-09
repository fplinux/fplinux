# SPDX-License-Identifier: GPL-2.0-only
"""Install the exact Python wheels declared for the quality image."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
import tomllib
import zipfile
from pathlib import Path


def install_wheels(lock: Path, inputs: Path, destination: Path, *, offline: bool) -> None:
    """Verify saved or downloaded wheels before extracting their declared bytes."""
    with lock.open("rb") as stream:
        declarations = tomllib.load(stream)
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fplinux-quality-wheels-") as temporary:
        for record in declarations["input"]:
            cached_path = Path(record["destination"])
            if record["purpose"] != "container-source" or cached_path.suffix != ".whl":
                continue
            archive = Path(temporary) / cached_path.name
            if offline:
                source = inputs / "sources" / cached_path.relative_to("downloads/environment")
                shutil.copyfile(source, archive)
            else:
                subprocess.run(
                    ["curl", "-fsSL", "--retry", "3", "--output", str(archive), record["url"]],
                    check=True,
                )
            with archive.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != record["sha256"]:
                raise SystemExit(f"wheel SHA-256 mismatch: {record['key']}")
            if archive.stat().st_size != record["bytes"]:
                raise SystemExit(f"wheel size mismatch: {record['key']}")
            with zipfile.ZipFile(archive) as wheel:
                wheel.extractall(destination)


def main() -> None:
    """Install quality wheels from the explicit image-build inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--offline", choices=("0", "1"), required=True)
    args = parser.parse_args()
    install_wheels(args.lock, args.inputs, args.destination, offline=args.offline == "1")


if __name__ == "__main__":
    main()
