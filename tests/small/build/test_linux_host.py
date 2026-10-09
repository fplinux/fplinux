# SPDX-License-Identifier: GPL-2.0-only
"""Linux shared-library projection with a controlled readelf boundary."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.linux_host import _write_launchers, verify_linux_dependencies
from fplinux_cli.runtime.ssh_transport import _require_tool

if TYPE_CHECKING:
    from pathlib import Path


def _elf(path: Path, *, machine: int = 62) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray(20)
    header[:6] = b"\x7fELF\x02\x01"
    header[18:20] = machine.to_bytes(2, "little")
    path.write_bytes(header)
    return path


@pytest.mark.parametrize("library_directory", ["runtime/lib", "runtime/lib/python3.14"])
def test_dependency_requires_library_in_the_loader_search_directory(
    tmp_path: Path, library_directory: str
) -> None:
    """Stub readelf; filesystem placement and closure validation remain real."""
    program = _elf(tmp_path / "runtime/bin/python3.14")
    library = _elf(tmp_path / library_directory / "libpython3.14.so.1.0")
    inspected = subprocess.CompletedProcess(
        [], 0, " 0x0000000000000001 (NEEDED) Shared library: [libpython3.14.so.1.0]\n", ""
    )
    files = {
        "runtime/bin/python3.14": program,
        f"{library_directory}/libpython3.14.so.1.0": library,
    }
    with mock.patch("fplinux_cli.build.linux_host.subprocess.run", return_value=inspected):
        if library_directory == "runtime/lib":
            imports = verify_linux_dependencies(files)
            assert imports["runtime/bin/python3.14"] == ["libpython3.14.so.1.0"]
        else:
            with pytest.raises(SystemExit, match="Linux runtime library is missing"):
                verify_linux_dependencies(files)


def test_dependency_rejects_a_non_x86_64_runtime(tmp_path: Path) -> None:
    program = _elf(tmp_path / "runtime/bin/python3.14", machine=183)
    with pytest.raises(SystemExit, match="not a little-endian x86-64 ELF"):
        verify_linux_dependencies({"runtime/bin/python3.14": program})


def test_plain_standard_library_sources_need_no_elf_inspection(tmp_path: Path) -> None:
    module = tmp_path / "module.py"
    module.write_text("value = 42\n", encoding="utf-8")
    with mock.patch(
        "fplinux_cli.build.linux_host.subprocess.run", side_effect=AssertionError("readelf")
    ):
        assert verify_linux_dependencies({"runtime/lib/python3.14/module.py": module}) == {}


def test_sftp_preflight_finds_bundled_launcher_without_system_openssh(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    package = tmp_path / "extracted archive"
    _write_launchers(package)
    tools = package / "tools/linux"
    monkeypatch.setenv("PATH", str(tools))
    assert _require_tool("sftp") == str(tools / "sftp")


def test_sftp_launcher_selects_the_sibling_ssh_client(tmp_path: Path) -> None:
    """Replace the external loader with an argument recorder, without connecting to a peer."""
    package = tmp_path / "extracted archive"
    _write_launchers(package)
    runtime = package / "runtime"
    library = runtime / "lib"
    library.mkdir(parents=True)
    loader = library / "ld-musl-x86_64.so.1"
    loader.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n', encoding="utf-8")
    loader.chmod(0o755)
    result = subprocess.run(
        [str(package / "tools/linux/sftp"), "-h"],
        cwd=tmp_path,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout.splitlines() == [
        "--library-path",
        str(library),
        str(runtime / "bin/sftp"),
        "-S",
        str(package / "tools/linux/ssh"),
        "-h",
    ]
