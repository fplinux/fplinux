# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: SLF001 -- exercise the actual rootfs extraction callback.
"""Alpine archive extraction on a temporary filesystem, without a container."""

from __future__ import annotations

import io
import tarfile
import tempfile
from pathlib import Path

import pytest
from fplinux_cli.alpine import rootfs as alpine_builder


class AlpineArchiveTests:
    """Preserve Alpine links and rejections in the target rootfs tar filter."""

    def test_files_and_absolute_symlinks_survive_extraction(self) -> None:
        """An Alpine-rooted symlink keeps its target while regular data is extracted."""
        with tempfile.TemporaryDirectory() as temporary:
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
                archive.extractall(temporary, filter=alpine_builder._alpine_tar_filter)  # noqa: S202 -- subject under test.

            root = Path(temporary)
            assert ((root / "bin/busybox").read_bytes()) == (b"payload")
            assert ((root / "bin/busybox").stat().st_mode & 0o777) == (0o755)
            assert ((root / "bin/sh").readlink()) == (Path("/bin/busybox"))

    def test_parent_traversal_cannot_write_outside_the_extraction_root(self) -> None:
        """The target filter retains the standard data filter's path containment."""
        with tempfile.TemporaryDirectory() as temporary:
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
                pytest.raises(tarfile.OutsideDestinationError),
            ):
                archive.extractall(root, filter=alpine_builder._alpine_tar_filter)  # noqa: S202 -- subject under test.

            assert not ((root.parent / "outside").exists())

    def test_invalid_absolute_links_use_the_shared_error_prefix(self) -> None:
        """The target filter reports an escaping link with the CLI error prefix."""
        with tempfile.TemporaryDirectory() as temporary:
            link = tarfile.TarInfo("bin/sh")
            link.type = tarfile.SYMTYPE
            link.linkname = "/../outside"

            with pytest.raises(SystemExit) as raised:
                alpine_builder._alpine_tar_filter(link, temporary)

            assert (str(raised.value)) == (
                "fplinux: Alpine minirootfs link escapes the root: bin/sh"
            )
