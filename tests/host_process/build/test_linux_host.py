# SPDX-License-Identifier: GPL-2.0-only
"""Run the relocated Linux SSH launcher with a controlled runtime."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.host_tools.linux_host import _write_launchers
from fplinux_cli.runtime.ssh import transport

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def archive_with_recording_loader(tmp_path: Path) -> Path:
    """Replace the native ELF loader with an argument recorder."""
    package = tmp_path / "extracted archive"
    _write_launchers(package)
    library = package / "runtime/lib"
    library.mkdir(parents=True)
    loader = library / "ld-musl-x86_64.so.1"
    loader.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n', encoding="utf-8")
    loader.chmod(0o755)
    return package


def test_ssh_launcher_selects_the_relocated_runtime_with_literal_arguments(
    archive_with_recording_loader: Path, tmp_path: Path
) -> None:
    package = archive_with_recording_loader
    result = subprocess.run(
        [str(package / "tools/linux/ssh"), "-V"],
        cwd=tmp_path,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout.splitlines() == [
        "--library-path",
        str(package / "runtime/lib"),
        str(package / "runtime/bin/ssh"),
        "-V",
    ]


def test_sftp_uses_the_archive_ssh_without_system_openssh(
    archive_with_recording_loader: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    package = archive_with_recording_loader
    shell_tools = tmp_path / "shell-tools"
    shell_tools.mkdir()
    (shell_tools / "dirname").symlink_to("/usr/bin/dirname")
    monkeypatch.setenv("PATH", f"{package / 'tools/linux'}:{shell_tools}")
    session = {
        "private_key": tmp_path / "id_ed25519",
        "known_hosts": tmp_path / "known_hosts",
        "usb_serial": "fixture",
        "host_address": "10.23.45.1",
        "phone_address": "10.23.45.2",
    }

    result = transport._sftp(session, 'get "/tmp/saved" "saved"')  # noqa: SLF001

    assert result.returncode == 0, result.stderr
    arguments = result.stdout.splitlines()
    assert arguments[:3] == [
        "--library-path",
        str(package / "runtime/lib"),
        str(package / "runtime/bin/sftp"),
    ]
    ssh = arguments.index("-S")
    assert arguments[ssh + 1] == str(package / "tools/linux/ssh")
    assert "StrictHostKeyChecking=yes" in arguments
