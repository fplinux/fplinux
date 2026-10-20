# SPDX-License-Identifier: GPL-2.0-only
"""Execute the complete initial-root handoff with real shell and file copies."""

from __future__ import annotations

import shutil
import stat
from pathlib import Path

import pytest

from tests import ROOT
from tests.process import run_process

FIXTURES = ROOT / "tests/host_process/rootfs/fixtures/ramroot-init"


def install_shell(root: Path) -> None:
    """Give the isolated root the quality runtime's BusyBox and its musl loader."""
    binaries = root / "bin"
    binaries.mkdir(parents=True)
    runtime = root / "libexec"
    runtime.mkdir()
    shutil.copyfile("/bin/busybox", runtime / "busybox")
    (runtime / "busybox").chmod(0o755)
    (binaries / "sh").symlink_to("../libexec/busybox")
    loader = Path("/lib/ld-musl-x86_64.so.1")
    (root / "lib").mkdir()
    shutil.copy2(loader, root / loader.relative_to("/"))


@pytest.mark.parametrize("marker", [None, b"1\n"], ids=["ordinary-session", "preparation-session"])
@pytest.mark.parametrize(
    "transport", [None, b"rndis\n"], ids=["default-transport", "explicit-rndis"]
)
def test_runtime_configuration_survives_initial_root_handoff(
    tmp_path: Path, marker: bytes | None, transport: bytes | None
) -> None:
    """The unchanged script carries selected files into the root that executes init."""
    root = tmp_path / "initial-root"
    root.mkdir()
    install_shell(root)
    newroot = root / "newroot"
    newroot.mkdir()
    install_shell(newroot)
    for directory in ("dev", "proc", "sys", "run"):
        (root / directory).mkdir()
        (newroot / directory).mkdir()
    # Mount creation is simulated; the full production mount validation still runs.
    (root / "proc/mounts").write_text(
        "overlay /newroot overlay rw,index=on,redirect_dir=on 0 0\n", encoding="ascii"
    )
    configuration = root / "etc/fplinux"
    configuration.mkdir(parents=True)
    if transport is not None:
        (configuration / "usb-transport").write_bytes(transport)
        (configuration / "usb-transport").chmod(0o640)
    (configuration / "bootstrap-only").write_bytes(b"initial root only\n")
    (root / "initial-root-only").touch()
    if marker is not None:
        (configuration / "preparation").write_bytes(marker)
        (configuration / "preparation").chmod(0o600)
    firmware = root / "lib/firmware"
    firmware.mkdir()
    (firmware / "device.bin").write_bytes(b"phone firmware\n")
    shutil.copyfile(ROOT / "alpine/ramroot-init.sh", root / "init")
    shutil.copyfile(FIXTURES / "busybox-boundary.sh", root / "bin/busybox")
    (root / "bin/busybox").chmod(0o755)
    (newroot / "sbin").mkdir()
    shutil.copyfile(FIXTURES / "system-init.sh", newroot / "sbin/init")
    (newroot / "sbin/init").chmod(0o755)

    result = run_process(
        ["/bin/busybox", "chroot", str(root), "/bin/sh", "/init", "first-start", "--no-console"],
        name="run the complete initial-root script through its root handoff",
        timeout=5,
    )

    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    transport_line = (
        "transport=absent\n" if transport is None else f"transport={transport.decode('ascii')}"
    )
    preparation_line = "preparation=absent\n" if marker is None else "preparation=1\n"
    assert result.stdout == (
        "system-init\n"
        "argument=first-start\n"
        "argument=--no-console\n"
        f"{transport_line}"
        f"{preparation_line}"
    )
    copied = newroot / "etc/fplinux"
    if transport is None:
        assert not (copied / "usb-transport").exists()
    else:
        assert (copied / "usb-transport").read_bytes() == transport
        assert stat.S_IMODE((copied / "usb-transport").stat().st_mode) == 0o640
    if marker is None:
        assert not (copied / "preparation").exists()
    else:
        assert (copied / "preparation").read_bytes() == b"1\n"
        assert stat.S_IMODE((copied / "preparation").stat().st_mode) == 0o600
    assert not (copied / "bootstrap-only").exists()
    assert (newroot / "lib/firmware/device.bin").read_bytes() == b"phone firmware\n"
