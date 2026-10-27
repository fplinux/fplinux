# SPDX-License-Identifier: GPL-2.0-only
"""Host component contract for offline installation of the pinned runtime."""

from __future__ import annotations

import hashlib
import io
import os
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.environment import kern

if TYPE_CHECKING:
    import pytest


def test_saved_archive_installs_the_exact_binary_without_downloading(tmp_path: Path) -> None:
    """Verified saved release bytes install the executable when no runtime is present."""
    executable = b"static runtime fixture\n"
    archive = tmp_path / ".data/downloads/sources/kern/kern.tar.gz"
    archive.parent.mkdir(parents=True)
    with tarfile.open(archive, "w:gz") as bundle:
        member = tarfile.TarInfo("kern")
        member.size = len(executable)
        bundle.addfile(member, io.BytesIO(executable))
    lock = {
        "kern": {
            "version": "1.0.0",
            "archive_url": "https://example.invalid/kern.tar.gz",
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "binary_sha256": hashlib.sha256(executable).hexdigest(),
            "destination": "downloads/sources/kern/kern.tar.gz",
        }
    }
    with (
        mock.patch.object(kern, "ROOT", tmp_path),
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
    ):
        installed = Path(kern.install_kern(lock, offline=True))
    assert installed == tmp_path / ".data/tools/kern/kern"
    assert installed.read_bytes() == b"static runtime fixture\n"
    assert installed.stat().st_mode & 0o777 == 0o755


def test_kern_environment_uses_its_captured_root_and_preserves_the_host_phone_store(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    shared_phone_store = tmp_path / "personal-data"
    selected = tmp_path / "managed"
    monkeypatch.setenv("XDG_DATA_HOME", str(shared_phone_store))
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(tmp_path / "later-selection"))

    environment = kern.kern_environment(cache=selected)

    assert environment["XDG_CACHE_HOME"] == str(tmp_path / "managed/kern/cache")
    assert environment["XDG_DATA_HOME"] == str(tmp_path / "managed/kern/data")
    assert environment["XDG_CONFIG_HOME"] == str(tmp_path / "managed/kern/config")
    assert "FPLINUX_DATA_DIR" not in environment
    assert os.environ["XDG_DATA_HOME"] == str(shared_phone_store)
    assert not shared_phone_store.exists()
    assert not (tmp_path / "later-selection").exists()
