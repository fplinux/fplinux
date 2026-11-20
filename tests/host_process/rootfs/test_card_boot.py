# SPDX-License-Identifier: GPL-2.0-only
"""Observe the card's running root and keep confirmation gated by the GUI."""

from __future__ import annotations

import shutil
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime.connection import read_running_build

from tests import ROOT
from tests.host_process.rootfs.test_ramroot_init import install_shell
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

_BUILD = "0123456789ab"
_CARD = "run/fplinux-card/system"
_BOOT = "/usr/libexec/fplinux/card-boot"


def _write(root: Path, path: str, data: bytes) -> None:
    destination = root / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


@pytest.fixture
def card_root(tmp_path: Path) -> Path:
    """Use a real shell and slot writer with kernel state represented by files."""
    root = tmp_path / "root"
    install_shell(root)
    for applet in ("busybox", "sed", "uname"):
        (root / "bin" / applet).symlink_to("../libexec/busybox")
    _write(root, "sys/firmware/devicetree/base/chosen/fplinux,slot", b"b\x00")
    _write(root, "etc/os-release", f"BUILD_ID={_BUILD}\n".encode())
    _write(
        root,
        "proc/self/mountinfo",
        b"39 1 179:10 /system/b/root / rw - ext4 /dev/mmcblk1p2 rw\n"
        b"40 39 0:26 / /run rw - tmpfs tmpfs rw\n"
        b"41 40 179:10 / /run/fplinux-card rw - ext4 /dev/mmcblk1p2 rw\n",
    )
    _write(root, f"{_CARD}/active", b"b\n")
    _write(root, f"{_CARD}/target", b"nokia-ta1618\n")
    _write(root, f"{_CARD}/b/root/etc/os-release", (root / "etc/os-release").read_bytes())
    _write(root, f"{_CARD}/b/FPLINUX.ITB", b"verified test payload")
    libexec = root / "usr/libexec/fplinux"
    libexec.mkdir(parents=True)
    shutil.copyfile(
        ROOT / "alpine/aports/fplinux-microsd-root/fplinux-card-boot", libexec / "card-boot"
    )
    (libexec / "card-boot").chmod(0o755)
    shutil.copyfile(ROOT / "scripts/fplinux_cli/runtime/card-slots.sh", libexec / "card-slots.sh")
    return root


def _run(root: Path, command: str) -> subprocess.CompletedProcess[str]:
    return run_process(
        ["busybox", "chroot", str(root), "/bin/sh", "-c", command],
        name="observe the actual system-card boot helper",
        timeout=10,
        env={"PATH": "/bin"},
    )


def test_pc_inspection_does_not_confirm_the_card(card_root: Path) -> None:
    def execute(
        _session: dict[str, object], command: str, **_options: object
    ) -> subprocess.CompletedProcess[str]:
        return _run(card_root, command)

    observed = read_running_build((SimpleNamespace(run_remote=execute), {}), inspect_card=True)
    assert observed.card_slot == "b"
    assert observed.build_id == _BUILD
    assert not (card_root / _CARD / "b/ready").exists()


def test_boot_confirms_only_after_the_graphical_session_is_ready(card_root: Path) -> None:
    before = _run(card_root, f"{_BOOT} confirm")
    assert before.returncode == 1
    assert "graphical phone session" in before.stderr
    _write(card_root, "run/fplinux-gui/ready", b"ready\n")
    ready = _run(card_root, f"{_BOOT} confirm")
    assert ready.returncode == 0, ready.stderr
    marker = card_root / _CARD / "b/ready"
    assert marker.read_text().split()[0] == _BUILD
    assert (card_root / _CARD / "active").read_bytes() == b"b\n"


def test_boot_rejects_backing_from_a_different_device(card_root: Path) -> None:
    mountinfo = card_root / "proc/self/mountinfo"
    mountinfo.write_bytes(mountinfo.read_bytes().replace(b"41 40 179:10", b"41 40 179:18"))
    result = _run(card_root, f"{_BOOT} inspect")
    assert result.returncode == 1
    assert "mounted root does not match" in result.stderr
