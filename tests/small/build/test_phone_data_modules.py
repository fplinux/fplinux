# SPDX-License-Identifier: GPL-2.0-only
"""Verify the scoped pure-package projection without installing Python distributions."""

from __future__ import annotations

import hashlib
import io
import tarfile
import zipfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.host_tools.phone_data import prepare_python_modules

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def reader_originals(tmp_path: Path) -> tuple[Path, Path]:
    """Own minimal declared distribution archives with independent source and notice bytes."""
    root, cache = tmp_path / "source", tmp_path / "cache"
    root.mkdir()
    records = []
    for name, version in (("ext4", "1.4.1"), ("cachetools", "7.1.1")):
        source_id = f"python_{name}"
        filename = f"{name}-{version}-py3-none-any.whl"
        archive = cache / "downloads/sources" / source_id / filename
        archive.parent.mkdir(parents=True)
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr(f"{name}/__init__.py", f"NAME = '{name}'\n")
            output.writestr(f"{name}/reader.py", "VALUE = b'unchanged source'\n")
            output.writestr(f"{name}-{version}.dist-info/licenses/LICENSE", f"{name} notice\n")
        records.append((source_id, version, archive))
    archive = cache / "downloads/sources/python_crcmod/crcmod-1.7.tar.gz"
    archive.parent.mkdir(parents=True)
    with tarfile.open(archive, "w:gz") as output:
        for name in ("__init__.py", "crcmod.py", "_crcfunpy.py", "predefined.py", "LICENSE"):
            contents = b"original crcmod notice\n" if name == "LICENSE" else b"VALUE = 42\n"
            relative = name if name == "LICENSE" else f"python3/crcmod/{name}"
            member = tarfile.TarInfo(f"crcmod-1.7/{relative}")
            member.size = len(contents)
            output.addfile(member, io.BytesIO(contents))
    records.append(("python_crcmod", "1.7", archive))
    (root / "sources.lock.toml").write_text(
        "\n".join(
            f'[{name}]\nversion = "{version}"\nname = "{path.name}"\n'
            f'url = "https://example.invalid/{path.name}"\nalgorithm = "sha256"\n'
            f'checksum = "{hashlib.sha256(path.read_bytes()).hexdigest()}"\n'
            f"bytes = {path.stat().st_size}\n"
            for name, version, path in records
        ),
        encoding="utf-8",
    )
    return root, cache


def test_pure_projection_preserves_sources_and_required_notices(
    tmp_path: Path, reader_originals: tuple[Path, Path]
) -> None:
    root, cache = reader_originals
    destination = tmp_path / "delivery"
    files = prepare_python_modules(destination, root=root, cache=cache)

    assert files["scripts/ext4/reader.py"].read_bytes() == b"VALUE = b'unchanged source'\n"
    assert files["scripts/cachetools/__init__.py"].read_bytes() == b"NAME = 'cachetools'\n"
    assert files["scripts/crcmod/_crcfunpy.py"].read_bytes() == b"VALUE = 42\n"
    assert files["licenses/phone-data/ext4-LICENSE.txt"].read_bytes() == b"ext4 notice\n"
    assert (
        files["licenses/phone-data/crcmod-LICENSE.txt"].read_bytes() == b"original crcmod notice\n"
    )


def test_offline_projection_requires_admitted_original_bytes(
    tmp_path: Path, reader_originals: tuple[Path, Path]
) -> None:
    root, cache = reader_originals
    (cache / "downloads/sources/python_ext4/ext4-1.4.1-py3-none-any.whl").write_bytes(
        b"incomplete"
    )
    with pytest.raises(SystemExit):
        prepare_python_modules(tmp_path / "delivery", root=root, cache=cache)
    assert not (tmp_path / "delivery/scripts/ext4").exists()
