# SPDX-License-Identifier: GPL-2.0-only
"""Collect active source bytes from a controlled checkout without private data."""

from __future__ import annotations

import io
import subprocess
import tarfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.manifests.loaders import LoaderSource
from fplinux_cli.packaging.sources import (
    _loader_license_notices,
    project_source_files,
)

if TYPE_CHECKING:
    from pathlib import Path


def test_source_companion_keeps_active_sources_and_excludes_private_runtime_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Use real Git discovery and current filesystem bytes, without a source-list snapshot."""
    subprocess.run(["git", "init", "--quiet", str(tmp_path)], check=True)
    (tmp_path / ".gitignore").write_text("/.cache/\n/AGENTS.md\n")
    source = tmp_path / "common/boot.py"
    source.parent.mkdir()
    source.write_bytes(b"original tracked source\n")
    source.chmod(0o755)
    subprocess.run(["git", "-C", str(tmp_path), "add", "common/boot.py"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(tmp_path),
            "-c",
            "user.name=Fixture",
            "-c",
            "user.email=fixture@example.invalid",
            "commit",
            "--quiet",
            "-m",
            "Initial source",
        ],
        check=True,
    )
    source.write_bytes(b"active edited source\n")
    (tmp_path / "common/new.py").write_bytes(b"new source\n")
    (tmp_path / "common/id_ed25519.pem").write_bytes(b"private key\n")
    (tmp_path / "common/.env").write_bytes(b"private settings\n")
    (tmp_path / "common/cache.bin").write_bytes(b"personal phone image\n")
    (tmp_path / ".cache").mkdir()
    (tmp_path / ".cache/private.py").write_bytes(b"private evidence\n")
    (tmp_path / "AGENTS.md").write_bytes(b"private instructions\n")
    monkeypatch.setattr(common, "ROOT", tmp_path)

    payload = project_source_files()

    assert payload.files["project/common/boot.py"] == b"active edited source\n"
    assert payload.files["project/common/new.py"] == b"new source\n"
    assert "project/common/boot.py" in payload.executables
    all_contents = b"".join(payload.files.values())
    assert b"private key" not in all_contents
    assert b"private settings" not in all_contents
    assert b"personal phone image" not in all_contents
    assert b"private evidence" not in all_contents
    assert b"private instructions" not in all_contents


def _source_archive(members: dict[str, bytes]) -> bytes:
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode="w:gz") as archive:
        for name, contents in members.items():
            member = tarfile.TarInfo(name)
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))
    return payload.getvalue()


def test_loader_distribution_retains_exact_upstream_license_files() -> None:
    """Runtime notices preserve original bytes while full sources stay in the companion."""
    sources = tuple(
        LoaderSource(
            role, role, "https://example.invalid/source", "a" * 64, f"loaders/{role}.tar.gz", role
        )
        for role in ("fdl1", "fdl2", "sml")
    )
    files = {
        "upstream/loaders/fdl1.tar.gz": _source_archive({"fdl1/main.c": b"original copyright\n"}),
        "upstream/loaders/fdl2.tar.gz": _source_archive(
            {
                "fdl2/Licenses/README": b"original license explanation\n",
                "fdl2/Licenses/gpl-2.0.txt": b"original GPL terms\n",
                "fdl2/main.c": b"original board code\n",
            }
        ),
        "upstream/loaders/sml.tar.gz": _source_archive(
            {
                "sml/license.md": b"original monitor license\n",
                "sml/main.c": b"monitor code\n",
            }
        ),
    }
    notices = _loader_license_notices(sources, files)
    assert notices == {
        "licenses/loaders/fdl2/README": b"original license explanation\n",
        "licenses/loaders/fdl2/gpl-2.0.txt": b"original GPL terms\n",
        "licenses/loaders/sml/license.md": b"original monitor license\n",
    }


@pytest.mark.parametrize("role", ["fdl2", "sml"])
def test_loader_distribution_rejects_an_archive_missing_its_license_files(role: str) -> None:
    source = LoaderSource(
        role, role, "https://example.invalid/source", "a" * 64, f"loaders/{role}.tar.gz", role
    )
    files = {f"upstream/loaders/{role}.tar.gz": _source_archive({f"{role}/main.c": b"source"})}
    with pytest.raises(SystemExit, match=r"license.*missing"):
        _loader_license_notices((source,), files)
