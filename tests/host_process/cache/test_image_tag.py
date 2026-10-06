# SPDX-License-Identifier: GPL-2.0-only
"""Host copy publication across a real subordinate UID/GID namespace."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.environment import image_store as kern


def _metadata(path: Path) -> tuple[int, int, int]:
    """Return the ownership and permission contract of one file or directory."""
    metadata = path.stat()
    return metadata.st_uid, metadata.st_gid, stat.S_IMODE(metadata.st_mode)


class ImageTagProcessTests(unittest.TestCase):
    """A fake Kern delegates copying to real cp; it does not exercise the image store."""

    def test_tag_keeps_owned_directory_and_executable_metadata(self) -> None:
        """Publication preserves another mapped user's ownership and set-id modes."""
        unshare = shutil.which("unshare")
        if unshare is None:
            self.skipTest("subordinate namespace test requires unshare")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            prepare = (
                "import os, pathlib, sys; root=pathlib.Path(sys.argv[1]); "
                "directory=root/'builder'; directory.mkdir(); "
                "tool=root/'compiler'; tool.write_bytes(b'build tool\\n'); "
                "os.chown(directory, 1000, 1000); os.chmod(directory, 0o2755); "
                "os.chown(tool, 1000, 1000); os.chmod(tool, 0o6755)"
            )
            prepared = subprocess.run(
                [
                    unshare,
                    "--map-auto",
                    "--map-root-user",
                    "--",
                    sys.executable,
                    "-c",
                    prepare,
                    str(source),
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            if prepared.returncode:
                self.skipTest("subordinate namespace is unavailable: " + prepared.stderr.strip())
            self.assertNotEqual((source / "compiler").stat().st_uid, os.geteuid())
            expected_directory = _metadata(source / "builder")
            expected_tool = _metadata(source / "compiler")

            fake_kern = root / "fake-kern"
            fake_kern.write_text(
                f"#!{sys.executable}\n"
                "import pathlib, subprocess, sys\n"
                "if len(sys.argv) != 4 or sys.argv[1] != 'tag': sys.exit(2)\n"
                "source=pathlib.Path(sys.argv[2]); destination=pathlib.Path(sys.argv[3])\n"
                "destination.mkdir()\n"
                "sys.exit(subprocess.run(['cp', '-a', '--', str(source) + '/.', "
                "str(destination)], check=False).returncode)\n"
            )
            fake_kern.chmod(0o755)
            destination = root / "published"
            with mock.patch.object(kern, "ROOT", root):
                kern.tag_image(str(fake_kern), str(source), str(destination))

            self.assertEqual((destination / "compiler").read_bytes(), b"build tool\n")
            self.assertEqual(_metadata(destination / "builder"), expected_directory)
            self.assertEqual(_metadata(destination / "compiler"), expected_tool)


if __name__ == "__main__":
    unittest.main()
