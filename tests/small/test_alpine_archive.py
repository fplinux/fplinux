# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: SLF001 -- exercise the actual host and rootfs extraction callbacks.
"""Alpine archive extraction on a temporary filesystem, without a container."""

from __future__ import annotations

import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from fplinux_cli import alpine_builder
from fplinux_cli.environment import kern


class AlpineArchiveTests(unittest.TestCase):
    """Preserve Alpine links and the rejection behavior of both archive consumers."""

    def test_files_and_absolute_symlinks_survive_extraction(self) -> None:
        """An Alpine-rooted symlink keeps its target while regular data is extracted."""
        for archive_filter in (kern._alpine_tar_filter, alpine_builder._alpine_tar_filter):
            with self.subTest(consumer=archive_filter), tempfile.TemporaryDirectory() as temporary:
                archive_bytes = io.BytesIO()
                with tarfile.open(fileobj=archive_bytes, mode="w") as archive:
                    payload = tarfile.TarInfo("bin/busybox")
                    payload.size = 7
                    payload.mode = 0o755
                    archive.addfile(payload, io.BytesIO(b"payload"))
                    link = tarfile.TarInfo("bin/sh")
                    link.type = tarfile.SYMTYPE
                    link.linkname = "/bin/busybox"
                    archive.addfile(link)
                archive_bytes.seek(0)

                with tarfile.open(fileobj=archive_bytes) as archive:
                    archive.extractall(temporary, filter=archive_filter)  # noqa: S202 -- subject under test.

                root = Path(temporary)
                self.assertEqual((root / "bin/busybox").read_bytes(), b"payload")
                self.assertEqual((root / "bin/busybox").stat().st_mode & 0o777, 0o755)
                self.assertEqual((root / "bin/sh").readlink(), Path("/bin/busybox"))

    def test_parent_traversal_cannot_write_outside_the_extraction_root(self) -> None:
        """Both consumers retain the standard data filter's path containment."""
        for archive_filter in (kern._alpine_tar_filter, alpine_builder._alpine_tar_filter):
            with self.subTest(consumer=archive_filter), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary) / "root"
                root.mkdir()
                archive_bytes = io.BytesIO()
                with tarfile.open(fileobj=archive_bytes, mode="w") as archive:
                    payload = tarfile.TarInfo("../outside")
                    payload.size = 7
                    archive.addfile(payload, io.BytesIO(b"payload"))
                archive_bytes.seek(0)

                with (
                    tarfile.open(fileobj=archive_bytes) as archive,
                    self.assertRaises(tarfile.OutsideDestinationError),
                ):
                    archive.extractall(root, filter=archive_filter)  # noqa: S202 -- subject under test.

                self.assertFalse((root.parent / "outside").exists())

    def test_invalid_absolute_links_keep_each_consumers_error_prefix(self) -> None:
        """The host CLI and in-container builder retain their distinct diagnostics."""
        cases = (
            (kern._alpine_tar_filter, "fplinux: "),
            (alpine_builder._alpine_tar_filter, "build failed: "),
        )
        for archive_filter, prefix in cases:
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as temporary:
                link = tarfile.TarInfo("bin/sh")
                link.type = tarfile.SYMTYPE
                link.linkname = "/../outside"

                with self.assertRaises(SystemExit) as raised:
                    archive_filter(link, temporary)

                self.assertEqual(
                    str(raised.exception),
                    prefix + "Alpine minirootfs link escapes the root: bin/sh",
                )


if __name__ == "__main__":
    unittest.main()
