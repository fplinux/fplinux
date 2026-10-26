# SPDX-License-Identifier: GPL-2.0-only
"""Host component contract for offline installation of the pinned runtime."""

from __future__ import annotations

import hashlib
import io
import tarfile
from pathlib import Path
from unittest import mock

from fplinux_cli.environment import kern


def test_saved_archive_installs_the_exact_binary_without_downloading(tmp_path: Path) -> None:
    """Verified saved release bytes install the executable when no runtime is present."""
    executable = b"static runtime fixture\n"
    archive = tmp_path / ".cache/downloads/sources/kern/kern.tar.gz"
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
    assert installed == tmp_path / ".cache/tools/kern/kern"
    assert installed.read_bytes() == b"static runtime fixture\n"
    assert installed.stat().st_mode & 0o777 == 0o755
