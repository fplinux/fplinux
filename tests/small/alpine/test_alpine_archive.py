# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: SLF001 -- exercise the actual host and rootfs extraction callbacks.
"""Alpine archive extraction on a temporary filesystem, without a container."""

from __future__ import annotations

import io
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine import rootfs as alpine_builder
from fplinux_cli.environment import setup as kern

if TYPE_CHECKING:
    from collections.abc import Callable

EXTRACTION_FILTERS = [
    pytest.param(kern._alpine_tar_filter, id="environment"),
    pytest.param(alpine_builder._alpine_tar_filter, id="rootfs"),
]


@pytest.mark.parametrize("archive_filter", EXTRACTION_FILTERS)
class AlpineArchiveTests:
    """Preserve Alpine links and rejections in each extraction module's tar filter."""

    def test_files_and_absolute_symlinks_survive_extraction(
        self, archive_filter: Callable[[tarfile.TarInfo, str], tarfile.TarInfo | None]
    ) -> None:
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
                archive.extractall(temporary, filter=archive_filter)  # noqa: S202 -- subject under test.

            root = Path(temporary)
            assert ((root / "bin/busybox").read_bytes()) == (b"payload")
            assert ((root / "bin/busybox").stat().st_mode & 0o777) == (0o755)
            assert ((root / "bin/sh").readlink()) == (Path("/bin/busybox"))

    def test_parent_traversal_cannot_write_outside_the_extraction_root(
        self, archive_filter: Callable[[tarfile.TarInfo, str], tarfile.TarInfo | None]
    ) -> None:
        """Both modules' filters retain the standard data filter's path containment."""
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
                archive.extractall(root, filter=archive_filter)  # noqa: S202 -- subject under test.

            assert not ((root.parent / "outside").exists())

    def test_invalid_absolute_links_use_the_shared_error_prefix(
        self, archive_filter: Callable[[tarfile.TarInfo, str], tarfile.TarInfo | None]
    ) -> None:
        """Both modules' filters report an escaping link with the CLI error prefix."""
        with tempfile.TemporaryDirectory() as temporary:
            link = tarfile.TarInfo("bin/sh")
            link.type = tarfile.SYMTYPE
            link.linkname = "/../outside"

            with pytest.raises(SystemExit) as raised:
                archive_filter(link, temporary)

            assert (str(raised.value)) == (
                "fplinux: Alpine minirootfs link escapes the root: bin/sh"
            )
