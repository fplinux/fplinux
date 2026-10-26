# SPDX-License-Identifier: GPL-2.0-only
"""Project the pinned pure Python filesystem reader for checkout and archive consumers."""

from __future__ import annotations

import tarfile
import zipfile
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import resolve_source

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.manifests.source_catalog import ResolvedSource

_SOURCES = ("python_ext4", "python_cachetools", "python_crcmod")


def preparation_sources(root: Path) -> tuple[ResolvedSource, ...]:
    """Select only the three originals used by the saved eMMC filesystem reader."""
    return tuple(resolve_source(root, name) for name in _SOURCES)


def _write(destination: Path, relative: str, contents: bytes) -> Path:
    path = destination / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(contents)
    return path


def _wheel(archive: Path, destination: Path, package: str, version: str) -> dict[str, Path]:
    files = {}
    with zipfile.ZipFile(archive) as source:
        for name in source.namelist():
            if name.startswith(f"{package}/") and not name.endswith("/"):
                relative = f"scripts/{name}"
                files[relative] = _write(destination, relative, source.read(name))
        license_path = f"{package}-{version}.dist-info/licenses/LICENSE"
        relative = f"licenses/phone-data/{package}-LICENSE.txt"
        files[relative] = _write(destination, relative, source.read(license_path))
    if f"scripts/{package}/__init__.py" not in files:
        fail(f"phone-data package is incomplete: {package}")
    return files


def prepare_python_modules(
    destination: Path, *, root: Path, cache: Path, offline: bool
) -> dict[str, Path]:
    """Read verified originals without installing packages or compiling native extensions."""
    sources = {source.id: source for source in preparation_sources(root)}
    files = {}
    for package, source_id in (("ext4", "python_ext4"), ("cachetools", "python_cachetools")):
        source = sources[source_id]
        files.update(
            _wheel(source.acquire(cache, offline=offline), destination, package, source.version)
        )
    source = sources["python_crcmod"]
    with tarfile.open(source.acquire(cache, offline=offline)) as archive:
        prefix = f"crcmod-{source.version}/"
        selected = {
            **{
                f"scripts/crcmod/{name}": f"{prefix}python3/crcmod/{name}"
                for name in ("__init__.py", "crcmod.py", "_crcfunpy.py", "predefined.py")
            },
            "licenses/phone-data/crcmod-LICENSE.txt": f"{prefix}LICENSE",
        }
        for relative, name in selected.items():
            member = archive.getmember(name)
            stream = archive.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"phone-data source is not a regular file: {name}")
            files[relative] = _write(destination, relative, stream.read())
    return files
