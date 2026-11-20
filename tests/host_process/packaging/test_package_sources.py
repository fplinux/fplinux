# SPDX-License-Identifier: GPL-2.0-only
"""Collect source companion files through real Git discovery."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.packaging.sources import project_source_files

if TYPE_CHECKING:
    from pathlib import Path

    import pytest


def test_source_companion_keeps_active_sources_and_excludes_private_runtime_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Use real Git discovery and current filesystem bytes, without a source-list snapshot."""
    subprocess.run(["git", "init", "--quiet", str(tmp_path)], check=True)
    (tmp_path / ".gitignore").write_text("/.data/\n/AGENTS.md\n")
    (tmp_path / "VERSION").write_bytes(b"1.0.0\n")
    source = tmp_path / "common/boot.py"
    source.parent.mkdir()
    source.write_bytes(b"original tracked source\n")
    source.chmod(0o755)
    subprocess.run(["git", "-C", str(tmp_path), "add", "VERSION", "common/boot.py"], check=True)
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
    (tmp_path / ".data").mkdir()
    (tmp_path / ".data/generated.py").write_bytes(b"managed generated data\n")
    (tmp_path / ".private").mkdir()
    (tmp_path / ".private/manual.py").write_bytes(b"private evidence\n")
    selected = tmp_path / "common/generated"
    selected.mkdir()
    (selected / "runtime.py").write_bytes(b"selected managed data\n")
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(selected))
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
    assert b"managed generated data" not in all_contents
    assert b"selected managed data" not in all_contents
    assert b"private instructions" not in all_contents
