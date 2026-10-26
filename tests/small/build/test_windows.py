# SPDX-License-Identifier: GPL-2.0-only
"""Windows dependency projection and mocked inspection of DLL closure."""

from __future__ import annotations

import io
import subprocess
import zipfile
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.host_tools.windows import (
    _extract_zip,
    _native_tool_recipes,
    verify_windows_dependencies,
)
from fplinux_cli.manifests.source_catalog import acquire_source

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def windows_original(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, str]:
    """Resolve the Windows consumer through one canonical original owner."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    (tmp_path / "sources.lock.toml").write_text(
        '[windows]\nversion="1"\nname="windows.zip"\n'
        'url="https://example.invalid/windows.zip"\nalgorithm="sha256"\n'
        'checksum="5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"\n'
        "bytes=6\n"
    )
    return {"source_id": "windows"}


def test_locked_windows_download_can_be_reused_without_network(
    tmp_path: Path, windows_original: dict[str, str]
) -> None:
    """Replace the HTTPS response; checksum and offline cache behavior remain real."""
    lock = windows_original
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"hello\n")):
        path = acquire_source(common.ROOT, lock["source_id"], tmp_path, offline=False)
    assert path.read_bytes() == b"hello\n"
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        assert acquire_source(common.ROOT, lock["source_id"], tmp_path, offline=True) == path


def test_missing_windows_download_fails_offline_without_network(
    tmp_path: Path, windows_original: dict[str, str]
) -> None:
    lock = windows_original
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(SystemExit, match="offline locked input is missing or mismatched"),
    ):
        acquire_source(common.ROOT, lock["source_id"], tmp_path, offline=True)
    assert not (tmp_path / "downloads/sources/windows/windows.zip").exists()


@pytest.mark.parametrize(
    ("platform", "nand_ecc", "expected"),
    [
        ("sc9820e", False, ["spd_dump"]),
        ("ums9117", False, ["spd_dump", "libc_server"]),
        ("ums9117", True, ["spd_dump", "libc_server", "ums9117-nand-ecc"]),
    ],
)
def test_native_windows_payload_uses_the_selected_platform_runtime_tools(
    platform: str, expected: list[str], *, nand_ecc: bool
) -> None:
    recipes = _native_tool_recipes(platform, nand_ecc=nand_ecc)

    assert [recipe["name"] for recipe in recipes] == expected


def test_native_tool_selection_uses_runtime_roles_instead_of_fixed_binary_names() -> None:
    config = {
        "host": {
            "runtime_tools": {"loader": "upload", "bridge": "bridge", "keyboard": "input"},
            "tools": [
                {"name": "upload", "type": "make-archive"},
                {"name": "bridge", "type": "make-archive"},
                {"name": "input", "type": "cc-libusb"},
                {"name": "report", "type": "make-archive"},
            ],
        }
    }
    with mock.patch("fplinux_cli.build.host_tools.windows.load_platform", return_value=config):
        recipes = _native_tool_recipes("selected", nand_ecc=False)

    assert [recipe["name"] for recipe in recipes] == ["upload", "bridge"]


def test_native_windows_selection_rejects_an_absent_required_decoder() -> None:
    config = {
        "host": {
            "runtime_tools": {"loader": "upload"},
            "tools": [{"name": "upload", "type": "make-archive"}],
        }
    }
    with (
        mock.patch("fplinux_cli.build.host_tools.windows.load_platform", return_value=config),
        pytest.raises(SystemExit, match="host tools are not declared: ums9117-nand-ecc"),
    ):
        _native_tool_recipes("selected", nand_ecc=True)


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
