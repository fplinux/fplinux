# SPDX-License-Identifier: GPL-2.0-only
"""Prepared package publication and toolchain license projection on real files."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

from fplinux_cli.build.host_tools.host_package import copy_mingw_licenses, publish_host_package

if TYPE_CHECKING:
    from pathlib import Path


def test_published_package_replaces_old_files_and_survives_preparation_cleanup(
    tmp_path: Path,
) -> None:
    package = tmp_path / "work/package"
    runtime = package / "runtime"
    runtime.mkdir(parents=True)
    runtime.chmod(0o750)
    program = runtime / "python3"
    program.write_bytes(b"prepared runtime")
    program.chmod(0o755)
    notice = package / "NOTICE.txt"
    notice.write_bytes(b"redistribution notice")
    notice.chmod(0o640)
    destination = tmp_path / "published"
    destination.mkdir()
    (destination / "previous-runtime").write_bytes(b"obsolete runtime")

    files = publish_host_package(
        package, destination, relative_paths=("runtime/python3", "NOTICE.txt")
    )
    shutil.rmtree(tmp_path / "work")

    assert files == {
        "runtime/python3": destination / "runtime/python3",
        "NOTICE.txt": destination / "NOTICE.txt",
    }
    assert files["runtime/python3"].read_bytes() == b"prepared runtime"
    assert files["runtime/python3"].stat().st_mode & 0o777 == 0o755
    assert files["NOTICE.txt"].read_bytes() == b"redistribution notice"
    assert files["NOTICE.txt"].stat().st_mode & 0o777 == 0o640
    assert (destination / "runtime").stat().st_mode & 0o777 == 0o750
    assert not (destination / "previous-runtime").exists()


def test_mingw_license_projection_preserves_compiler_and_runtime_notices(tmp_path: Path) -> None:
    toolchain = tmp_path / "toolchain"
    runtime = toolchain / "x86_64-w64-mingw32/share/mingw32"
    runtime.mkdir(parents=True)
    (runtime / "COPYING").write_bytes(b"runtime distribution terms")
    (runtime / "COPYING.MinGW-w64-runtime.txt").write_bytes(b"runtime copyright notices")
    (runtime / "COPYING.MinGW-w64.txt").write_bytes(b"header copyright notices")
    (toolchain / "LICENSE.TXT").write_bytes(b"compiler distribution terms")
    licenses = tmp_path / "licenses/windows-host"
    licenses.mkdir(parents=True)

    copy_mingw_licenses(toolchain, licenses)

    assert (licenses / "mingw-w64-COPYING").read_bytes() == b"runtime distribution terms"
    assert (licenses / "mingw-w64-COPYING.MinGW-w64-runtime.txt").read_bytes() == (
        b"runtime copyright notices"
    )
    assert (
        licenses / "mingw-w64-COPYING.MinGW-w64.txt"
    ).read_bytes() == b"header copyright notices"
    assert (licenses / "llvm-LICENSE.txt").read_bytes() == b"compiler distribution terms"
