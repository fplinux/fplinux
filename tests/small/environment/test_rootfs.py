# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for the base-rootfs extraction contract."""

from __future__ import annotations

import io
import os
import stat
import tarfile
from pathlib import Path

from fplinux_cli.environment.rootfs import extract_rootfs


def test_base_extraction_preserves_executable_modes_and_logical_owners(tmp_path: Path) -> None:
    """The preparation step retains archive metadata instead of the data filter defaults."""
    archive = tmp_path / "base.tar.gz"
    with tarfile.open(archive, "w:gz") as bundle:
        directory = tarfile.TarInfo("usr")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        directory.uid = os.getuid()
        directory.gid = os.getgid()
        bundle.addfile(directory)
        tool = tarfile.TarInfo("usr/tool")
        tool.mode = 0o4755
        tool.uid = os.getuid()
        tool.gid = os.getgid()
        tool.size = 5
        bundle.addfile(tool, io.BytesIO(b"tool\n"))
        alias = tarfile.TarInfo("tool")
        alias.type = tarfile.SYMTYPE
        alias.linkname = "/usr/tool"
        alias.uid = os.getuid()
        alias.gid = os.getgid()
        bundle.addfile(alias)
    rootfs = tmp_path / "rootfs"
    rootfs.mkdir()
    extract_rootfs(archive, rootfs)
    installed = rootfs / "usr/tool"
    assert installed.read_bytes() == b"tool\n"
    assert stat.S_IMODE(installed.stat().st_mode) == 0o4755
    assert (installed.stat().st_uid, installed.stat().st_gid) == (os.getuid(), os.getgid())
    assert (rootfs / "tool").readlink() == Path("/usr/tool")
