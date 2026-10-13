# SPDX-License-Identifier: GPL-2.0-only
"""Windows dependency projection and mocked inspection of DLL closure."""

from __future__ import annotations

import io
import subprocess
import zipfile
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.host_tools.windows import (
    _download,
    _extract_zip,
    verify_windows_dependencies,
)

if TYPE_CHECKING:
    from pathlib import Path


def test_locked_windows_download_can_be_reused_without_network(tmp_path: Path) -> None:
    """Replace the HTTPS response; checksum and offline cache behavior remain real."""
    lock = {
        "url": "https://example.invalid/windows.zip",
        "sha256": "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
        "size": 6,
        "name": "windows.zip",
    }
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"hello\n")):
        path = _download(lock, tmp_path, offline=False)
    assert path.read_bytes() == b"hello\n"
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        assert _download(lock, tmp_path, offline=True) == path


def test_missing_windows_download_fails_offline_without_network(tmp_path: Path) -> None:
    lock = {
        "url": "https://example.invalid/windows.zip",
        "sha256": "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
        "size": 6,
        "name": "windows.zip",
    }
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(SystemExit, match="offline locked input is missing or mismatched"),
    ):
        _download(lock, tmp_path, offline=True)
    assert not (tmp_path / "windows.zip").exists()


def test_openssh_projection_keeps_selected_client_and_notice(tmp_path: Path) -> None:
    source = tmp_path / "download.zip"
    with zipfile.ZipFile(source, "w") as archive:
        archive.writestr("OpenSSH-Win64/ssh.exe", b"client")
        archive.writestr("OpenSSH-Win64/LICENSE.txt", b"license")
        archive.writestr("OpenSSH-Win64/sshd.exe", b"server")
        archive.writestr("other/ssh.exe", b"other")
    destination = tmp_path / "package"
    _extract_zip(
        source,
        destination,
        prefix="OpenSSH-Win64/",
        members={"ssh.exe", "LICENSE.txt"},
    )
    assert (destination / "ssh.exe").read_bytes() == b"client"
    assert (destination / "LICENSE.txt").read_bytes() == b"license"
    assert not (destination / "sshd.exe").exists()
    assert not (destination / "other").exists()


@pytest.mark.parametrize("dll_directory", ["tools", "runtime"])
def test_dependency_requires_dll_beside_executable(tmp_path: Path, dll_directory: str) -> None:
    """Mock objdump output; application-local search policy remains the subject."""
    program = tmp_path / "tools/loader.exe"
    library = tmp_path / dll_directory / "libusb-1.0.dll"
    files = {"tools/loader.exe": program, f"{dll_directory}/libusb-1.0.dll": library}
    inspected = subprocess.CompletedProcess(
        [],
        0,
        "file format pei-x86-64\nDLL Name: KERNEL32.dll\nDLL Name: libusb-1.0.dll\n",
        "",
    )
    with mock.patch("fplinux_cli.build.host_tools.windows.subprocess.run", return_value=inspected):
        if dll_directory == "tools":
            imports = verify_windows_dependencies(files)
            assert imports["tools/loader.exe"] == ["KERNEL32.dll", "libusb-1.0.dll"]
        else:
            with pytest.raises(SystemExit, match=r"missing beside tools/loader\.exe: libusb"):
                verify_windows_dependencies(files)


def test_windows_dependency_rejects_wrong_architecture(tmp_path: Path) -> None:
    inspected = subprocess.CompletedProcess([], 0, "file format pei-i386", "")
    with (
        mock.patch("fplinux_cli.build.host_tools.windows.subprocess.run", return_value=inspected),
        pytest.raises(SystemExit, match=r"not an x86-64 PE: tools/loader\.exe"),
    ):
        verify_windows_dependencies({"tools/loader.exe": tmp_path / "loader.exe"})
