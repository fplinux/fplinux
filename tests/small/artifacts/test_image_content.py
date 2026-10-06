# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem contracts for installed-content fingerprints."""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.environment.image_content import image_content_digest


def installed_tree(root: Path) -> Path:
    """Prepare actual tool bytes, a shared library and an operational state marker."""
    compiler = root / "usr/bin/cc"
    compiler.parent.mkdir(parents=True)
    compiler.write_bytes(b"compiler one\n")
    compiler.chmod(0o755)
    library = root / "usr/lib/libc.so"
    library.parent.mkdir()
    library.write_bytes(b"shared library one\n")
    library.chmod(0o644)
    (root / "usr/bin/compiler").symlink_to("cc")
    state = root / "etc/fplinux-image-state"
    state.parent.mkdir()
    state.write_bytes(b"first recipe\nfirst generation\nfirst content\n")
    return compiler


class ImageContentTests(unittest.TestCase):
    """Installed file changes are causal; operational path and time are not."""

    def test_same_installed_content_ignores_location_time_and_generation(self) -> None:
        """Two installations have one identity despite their invocation-specific state."""
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first"
            second = Path(temporary) / "other-location"
            installed_tree(first)
            shutil.copytree(first, second, symlinks=True)
            (second / "etc/fplinux-image-state").write_bytes(b"new operational generation\n")
            for relative in ("etc/hostname", "etc/hosts", "etc/resolv.conf"):
                (second / relative).write_bytes(b"runtime host data\n")
            os.utime(second / "usr/bin/cc", (1000000000, 1000000000))
            self.assertEqual(image_content_digest(first), image_content_digest(second))

    def test_actual_compiler_and_library_bytes_change_identity(self) -> None:
        """A replaced installed compiler or linked library invalidates artifact recipes."""
        for relative in ("usr/bin/cc", "usr/lib/libc.so"):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                installed_tree(root)
                original = image_content_digest(root)
                (root / relative).write_bytes(b"changed effective input\n")
                self.assertNotEqual(image_content_digest(root), original)

    def test_tool_mode_and_symlink_resolution_change_identity(self) -> None:
        """Executable permissions and the selected tool target remain effective inputs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = installed_tree(root)
            original = image_content_digest(root)
            compiler.chmod(0o644)
            self.assertNotEqual(image_content_digest(root), original)
            compiler.chmod(0o755)
            alias = root / "usr/bin/compiler"
            alias.unlink()
            alias.symlink_to("other-compiler")
            self.assertNotEqual(image_content_digest(root), original)

    def test_installed_configuration_is_causal_but_scratch_is_not(self) -> None:
        """Only files supplied by the installed environment enter its tool identity."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed_tree(root)
            original = image_content_digest(root)
            scratch = root / "tmp"
            scratch.mkdir()
            (scratch / "log").write_bytes(b"setup output\n")
            self.assertEqual(image_content_digest(root), original)
            (root / "etc/compiler.conf").write_bytes(b"effective flags\n")
            self.assertNotEqual(image_content_digest(root), original)


if __name__ == "__main__":
    unittest.main()
