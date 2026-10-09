# SPDX-License-Identifier: GPL-2.0-only
"""Execute USB transport selection and writes with a directory-backed ConfigFS fake."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path

TRANSPORT = ROOT / "alpine/aports/fplinux-usb-gadget/fplinux-usb-transport.sh"
BOUNDARY = ROOT / "tests/host_process/rootfs/usb_configfs_boundary.sh"
CASE = ROOT / "tests/host_process/rootfs/usb_transport_case.sh"


@pytest.fixture
def configfs_root(tmp_path: Path) -> Path:
    """Replace automatic kernel directories and record function link ordering."""
    (tmp_path / "gadget/functions/gser.keyboard").mkdir(parents=True)
    (tmp_path / "gadget/configs/c.1/strings/0x409").mkdir(parents=True)
    descriptors = tmp_path / "gadget/os_desc"
    descriptors.mkdir()
    (descriptors / "use").write_text("0\n")
    binaries = tmp_path / "bin"
    binaries.mkdir()
    for command in ("mkdir", "ln"):
        (binaries / command).symlink_to(BOUNDARY)
    return tmp_path


def run_transport(root: Path, contents: str | None) -> subprocess.CompletedProcess[str]:
    """Source production shell code while replacing the external ConfigFS boundary."""
    if contents is not None:
        (root / "usb-transport").write_text(contents)
    return subprocess.run(
        ["sh", str(CASE), str(TRANSPORT), str(root)],
        env={"PATH": f"{root / 'bin'}:/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=False,
        timeout=5,
    )


@pytest.mark.parametrize("contents", [None, "ncm\n"], ids=["absent", "explicit-ncm"])
def test_default_ncm_keeps_keyboard_first_and_session_addresses(
    configfs_root: Path, contents: str | None
) -> None:
    """NCM remains the default without enabling Microsoft descriptors."""
    result = run_transport(configfs_root, contents)

    assert result.returncode == 0, result.stderr
    assert result.stdout == "ncm\n"
    gadget = configfs_root / "gadget"
    configuration = gadget / "configs/c.1"
    network = gadget / "functions/ncm.usb0"
    assert (network / "dev_addr").read_text() == "02:00:00:00:00:01\n"
    assert (network / "host_addr").read_text() == "02:00:00:00:00:02\n"
    assert (network / "ifname").read_text() == "usb%d\n"
    assert (configuration / "ncm.usb0").resolve() == network
    assert (configuration / "gser.keyboard").resolve() == gadget / "functions/gser.keyboard"
    assert (configuration / ".function-order").read_text() == "gser.keyboard\nncm.usb0\n"
    assert (configuration / "strings/0x409/configuration").read_text() == "Keyboard and NCM\n"
    assert (gadget / "os_desc/use").read_text() == "0\n"
    assert not (gadget / "os_desc/c.1").exists()
    assert not (gadget / "functions/rndis.usb0").exists()


def test_rndis_advertises_microsoft_compatible_id_and_keeps_session_addresses(
    configfs_root: Path,
) -> None:
    """RNDIS is first and advertises its OS identifier through the selected config."""
    result = run_transport(configfs_root, "rndis\n")

    assert result.returncode == 0, result.stderr
    assert result.stdout == "rndis\n"
    gadget = configfs_root / "gadget"
    configuration = gadget / "configs/c.1"
    network = gadget / "functions/rndis.usb0"
    assert (network / "dev_addr").read_text() == "02:00:00:00:00:01\n"
    assert (network / "host_addr").read_text() == "02:00:00:00:00:02\n"
    assert (network / "ifname").read_text() == "usb%d\n"
    assert (network / "class").read_text() == "ef\n"
    assert (network / "subclass").read_text() == "04\n"
    assert (network / "protocol").read_text() == "01\n"
    assert (configuration / "rndis.usb0").resolve() == network
    assert (configuration / "gser.keyboard").resolve() == gadget / "functions/gser.keyboard"
    assert (configuration / ".function-order").read_text() == "rndis.usb0\ngser.keyboard\n"
    assert (gadget / "os_desc/use").read_text() == "1\n"
    assert (gadget / "os_desc/b_vendor_code").read_text() == "0xcd\n"
    assert (gadget / "os_desc/qw_sign").read_text() == "MSFT100\n"
    assert (gadget / "os_desc/c.1").resolve() == configuration
    assert (network / "os_desc/interface.rndis/compatible_id").read_bytes() == b"RNDIS\n"
    assert (network / "os_desc/interface.rndis/sub_compatible_id").read_bytes() == b"\0" * 8
    assert not (gadget / "functions/ncm.usb0").exists()


def test_unknown_transport_stops_before_function_creation(configfs_root: Path) -> None:
    """An unsupported selection cannot silently choose a different host transport."""
    result = run_transport(configfs_root, "ecm\n")

    assert result.returncode != 0
    assert "unsupported USB transport: ecm" in result.stderr
    assert not (configfs_root / "gadget/functions/ncm.usb0").exists()
    assert not (configfs_root / "gadget/functions/rndis.usb0").exists()
    assert not (configfs_root / "gadget/configs/c.1/.function-order").exists()
