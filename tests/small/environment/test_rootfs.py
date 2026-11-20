# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for the base-rootfs extraction contract."""

from __future__ import annotations

import io
import stat
import tarfile
from typing import TYPE_CHECKING

from fplinux_cli.environment.rootfs import extract_rootfs

if TYPE_CHECKING:
    from pathlib import Path

    import pytest


def test_base_extraction_retains_modes_and_passes_logical_owners_to_chown(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Observe the ownership handed to tarfile, independent of test-process privileges."""
    ownership: dict[str, tuple[int | None, int | None, bool]] = {}

    def record_owner(
        _bundle: tarfile.TarFile,
        member: tarfile.TarInfo,
        _path: str,
        numeric_owner: bool,  # noqa: FBT001 -- tarfile can pass this positionally.
    ) -> None:
        ownership[member.name] = (member.uid, member.gid, numeric_owner)

    monkeypatch.setattr(tarfile.TarFile, "chown", record_owner)
    archive = tmp_path / "base.tar.gz"
    with tarfile.open(archive, "w:gz") as bundle:
        directory = tarfile.TarInfo("usr")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        directory.uid = 42
        directory.gid = 63
        bundle.addfile(directory)
        tool = tarfile.TarInfo("usr/tool")
        tool.mode = 0o4755
        tool.uid = 81
        tool.gid = 92
        tool.size = 5
        bundle.addfile(tool, io.BytesIO(b"tool\n"))
        alias = tarfile.TarInfo("tool")
        alias.type = tarfile.SYMTYPE
        alias.linkname = "/usr/tool"
        alias.uid = 123
        alias.gid = 456
        bundle.addfile(alias)
    rootfs = tmp_path / "rootfs"
    rootfs.mkdir()
    extract_rootfs(archive, rootfs)
    installed = rootfs / "usr/tool"
    assert installed.read_bytes() == b"tool\n"
    assert stat.S_IMODE(installed.stat().st_mode) == 0o4755
    assert stat.S_IMODE((rootfs / "usr").stat().st_mode) == 0o755
    assert (rootfs / "tool").readlink().as_posix() == "/usr/tool"
    assert ownership == {
        "usr": (42, 63, True),
        "usr/tool": (81, 92, True),
        "tool": (123, 456, True),
    }
