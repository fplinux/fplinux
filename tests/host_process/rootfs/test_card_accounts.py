# SPDX-License-Identifier: GPL-2.0-only
"""Verify the numeric identities shared by card slots and package installation."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.host_process.rootfs.test_ramroot_init import install_shell
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    "setup",
    [
        pytest.param(". /etc/init.d/fplinux-devices; start", id="boot"),
        pytest.param("/bin/sh /pre-install", id="apk-install"),
    ],
)
def test_account_creation_keeps_the_shared_home_identity(tmp_path: Path, setup: str) -> None:
    install_shell(tmp_path)
    for applet in ("addgroup", "adduser", "grep", "id"):
        (tmp_path / "bin" / applet).symlink_to("../libexec/busybox")
    devices = tmp_path / "etc" / "init.d"
    devices.mkdir(parents=True)
    account_files = devices.parent
    (account_files / "passwd").write_bytes(b"root:x:0:0:root:/root:/bin/sh\n")
    (account_files / "group").write_bytes(b"root:x:0:root\nusers:x:100:\n")
    (account_files / "shadow").touch()
    (tmp_path / "dev").mkdir()
    (tmp_path / "dev" / "null").touch()
    libexec = tmp_path / "usr" / "libexec" / "fplinux"
    libexec.mkdir(parents=True)

    aports = ROOT / "alpine" / "aports"
    shutil.copyfile(aports / "fplinux-base" / "fplinux-accounts", libexec / "accounts")
    (libexec / "accounts").chmod(0o755)
    shutil.copyfile(aports / "fplinux-base" / "fplinux-devices.initd", devices / "fplinux-devices")
    shutil.copyfile(
        aports / "fplinux-session" / "fplinux-session.pre-install", tmp_path / "pre-install"
    )
    result = run_process(
        [
            "busybox",
            "chroot",
            str(tmp_path),
            "/bin/sh",
            "-ec",
            f"{setup}; {setup}; id -u fplinux; id -g fplinux; id -G fplinux",
        ],
        name="create application accounts in a fresh root and repeat the operation",
        timeout=5,
        env={"PATH": "/bin"},
    )

    assert result.returncode == 0, result.stderr
    user_id, group_id, memberships = result.stdout.splitlines()
    assert user_id == "100"
    assert group_id == "102"
    assert set(memberships.split()) == {"101", "102"}
