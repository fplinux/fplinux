# SPDX-License-Identifier: GPL-2.0-only
"""Host copy publication across a real subordinate UID/GID namespace."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.environment import image_store, kern

from tests import ROOT
from tests.fixtures.executables import install_python_script

TAG_FIXTURES = ROOT / "tests/fixtures/processes"


def _metadata(path: Path) -> tuple[int, int, int]:
    """Return the ownership and permission contract of one file or directory."""
    metadata = path.stat()
    return metadata.st_uid, metadata.st_gid, stat.S_IMODE(metadata.st_mode)


class ImageTagProcessTests:
    """A fake Kern delegates copying to real cp; it does not exercise the image store."""

    def test_tag_keeps_owned_directory_and_executable_metadata(self) -> None:
        """Publication preserves another mapped user's ownership and set-id modes."""
        unshare = shutil.which("unshare")
        if unshare is None:
            pytest.skip("subordinate namespace test requires unshare")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            prepared = subprocess.run(
                [
                    unshare,
                    "--map-auto",
                    "--map-root-user",
                    "--",
                    sys.executable,
                    str(TAG_FIXTURES / "image_tag_prepare.py"),
                    str(source),
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            if prepared.returncode:
                pytest.skip("subordinate namespace is unavailable: " + prepared.stderr.strip())
            assert ((source / "compiler").stat().st_uid) != (os.geteuid())
            expected_directory = _metadata(source / "builder")
            expected_tool = _metadata(source / "compiler")

            fake_kern = root / "fake-kern"
            install_python_script(TAG_FIXTURES / "image_tag_provider.py", fake_kern)
            destination = root / "published"
            with (
                mock.patch.object(image_store, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
            ):
                image_store.tag_image(str(fake_kern), str(source), str(destination))

            assert ((destination / "compiler").read_bytes()) == (b"build tool\n")
            assert (_metadata(destination / "builder")) == (expected_directory)
            assert (_metadata(destination / "compiler")) == (expected_tool)
