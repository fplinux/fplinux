# SPDX-License-Identifier: GPL-2.0-only
"""Test selection from controlled Windows PnP and network snapshots."""

from __future__ import annotations

import copy
import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime import windows_host
from fplinux_cli.runtime.windows_host import bound_interface, exact_usb_devices

from tests.windows_host_support import (
    StubWindowsHost,
    admitted_windows,
    archive_files,
    endpoint_session,
    endpoint_snapshot,
)

if TYPE_CHECKING:
    from pathlib import Path


def test_archive_validation_uses_selected_runtime_and_separate_native_tools(
    tmp_path: Path,
) -> None:
    bundle = admitted_windows(archive_files(tmp_path))
    assert bundle.runtime["target"] == "demo"
    assert bundle.runtime["host_tools"] == {
        "loader": "tools/spd_dump.exe",
        "bridge": "tools/libc_server.exe",
    }
    assert bundle.tools["loader"] == tmp_path / "tools/spd_dump.exe"


def test_changed_native_tool_is_rejected_before_windows_discovery(tmp_path: Path) -> None:
    root = archive_files(tmp_path)
    (root / "tools/openssh/ssh.exe").write_bytes(b"different tool\n")
    with pytest.raises(SystemExit, match=r"Windows file checksum differs: tools/openssh/ssh.exe"):
        admitted_windows(root)


def test_sc9820e_archive_admits_its_compact_ram_image_without_a_bridge(tmp_path: Path) -> None:
    bundle = admitted_windows(archive_files(tmp_path, platform="sc9820e"))

    assert bundle.runtime["identity"]["platform"]["name"] == "sc9820e"
    assert bundle.runtime["host_tools"] == {"loader": "tools/spd_dump.exe"}
    assert "bridge" not in bundle.tools
    assert not (tmp_path / "tools/libc_server.exe").exists()
    assert (tmp_path / "image/ramboot.bin").read_bytes()[:8] == b"entry123"


def test_sc9820e_windows_archive_requires_an_initramfs(tmp_path: Path) -> None:
    bundle = admitted_windows(archive_files(tmp_path, platform="sc9820e"))
    runtime = copy.deepcopy(bundle.runtime)
    del runtime["adapter"]["regions"]["initramfs"]

    with pytest.raises(SystemExit, match="Windows archives require a RAM root filesystem"):
        windows_host.validate_archive(
            tmp_path, runtime, generation=bundle.generation, device_identity=bundle.device_identity
        )


def test_exact_device_discovery_excludes_composite_interfaces() -> None:
    snapshot = {
        "devices": [
            {"id": "USB\\VID_1782&PID_4D00\\phone-root"},
            {"id": "USB\\VID_1782&PID_4D00&MI_00\\phone-interface"},
            {"id": "USB\\VID_0525&PID_A4A6\\linux-root"},
        ]
    }
    assert exact_usb_devices(snapshot, 0x1782, 0x4D00) == ["USB\\VID_1782&PID_4D00\\phone-root"]


@pytest.mark.parametrize(
    ("usb_id", "present"),
    [
        ("USB\\VID_0525&PID_A4A6\\0123456789abcdef0123456789abcdef", True),
        ("usb\\vid_0525&pid_a4a6&rev_0100\\0123456789ABCDEF0123456789ABCDEF", True),
        ("USB\\VID_0525&PID_A4A6&MI_00\\0123456789abcdef0123456789abcdef", False),
        ("USB\\VID_0525&PID_A4A6\\another-phone", False),
        ("USB\\VID_1782&PID_4D00\\0123456789abcdef0123456789abcdef", False),
    ],
)
def test_usb_presence_requires_the_saved_device_root_without_a_network_index(
    tmp_path: Path, usb_id: str, *, present: bool
) -> None:
    host = StubWindowsHost({"devices": [{"id": usb_id}]})
    transport = windows_host.WindowsTransport(host, admitted_windows(archive_files(tmp_path)))

    assert transport.usb_present(endpoint_session(tmp_path / "session")) is present


@pytest.mark.parametrize(
    "compatible_id",
    [
        "USB\\MS_COMP_RNDIS",
        "USB\\MS_COMP_RNDIS&MS_SUBCOMP_5162001",
        "USB\\Class_EF&SubClass_04&Prot_01",
        "usb\\class_ef&subclass_04&prot_01",
    ],
)
def test_rndis_dhcp_endpoint_allows_an_unrelated_default_route(
    tmp_path: Path, compatible_id: str
) -> None:
    snapshot = endpoint_snapshot()
    snapshot["adapters"][0]["compatible"] = [compatible_id]
    assert bound_interface(snapshot, endpoint_session(tmp_path / "session")) == 17


@pytest.mark.parametrize("mismatch", ["serial", "mac", "dhcp-address"])
def test_another_usb_or_dhcp_endpoint_does_not_match(tmp_path: Path, mismatch: str) -> None:
    snapshot = endpoint_snapshot()
    adapter = snapshot["adapters"][0]
    if mismatch == "serial":
        adapter["lineage"][-1] = "USB\\VID_0525&PID_A4A6\\another-phone"
    elif mismatch == "mac":
        adapter["mac"] = "02-00-00-00-00-03"
    else:
        adapter["addresses"][0]["address"] = "10.23.45.2"
    assert bound_interface(snapshot, endpoint_session(tmp_path / "session")) is None


@pytest.mark.parametrize(
    "route",
    [
        {"index": 17, "prefix": "0.0.0.0/0", "next_hop": "10.23.45.2"},
        {"index": 3, "prefix": "10.23.45.2/32", "next_hop": "192.0.2.1"},
    ],
)
def test_phone_gateway_and_competing_phone_route_are_rejected(
    tmp_path: Path, route: dict[str, object]
) -> None:
    snapshot = endpoint_snapshot()
    snapshot["routes"].append(route)
    with pytest.raises(SystemExit, match=r"default route|competing route"):
        bound_interface(snapshot, endpoint_session(tmp_path / "session"))


@pytest.mark.parametrize(
    "compatible_id",
    [
        "USB\\MS_COMP_WINUSB",
        "USB\\Class_02&SubClass_06&Prot_00",
        "USB\\Class_EF&SubClass_04&Prot_00",
    ],
)
def test_a_matching_usb_endpoint_with_another_driver_is_rejected(
    tmp_path: Path, compatible_id: str
) -> None:
    snapshot = endpoint_snapshot()
    snapshot["adapters"][0]["compatible"] = [compatible_id]
    with pytest.raises(SystemExit, match="Windows RNDIS driver"):
        bound_interface(snapshot, endpoint_session(tmp_path / "session"))


def test_duplicate_exact_usb_network_endpoints_are_rejected(tmp_path: Path) -> None:
    snapshot = endpoint_snapshot()
    duplicate = copy.deepcopy(snapshot["adapters"][0])
    duplicate["index"] = 18
    snapshot["adapters"].append(duplicate)

    with pytest.raises(SystemExit, match="Multiple network adapters match this USB session"):
        bound_interface(snapshot, endpoint_session(tmp_path / "session"))


def test_host_preparation_progress_precedes_a_windows_discovery_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The real boot entry announces startup before its unavailable host boundary."""
    root = archive_files(tmp_path)
    capsule = root / "device-data.cpio"
    capsule.write_bytes(b"controlled capsule")

    class TimingOutWindowsHost(windows_host.WindowsHost):
        """Replace native runtime/user discovery with its observed timeout failure."""

        def __init__(self) -> None:
            raise subprocess.TimeoutExpired(["powershell.exe", "-File", "user.ps1"], 20)

    monkeypatch.setattr(windows_host, "WindowsHost", TimingOutWindowsHost)
    events: list[str] = []

    with pytest.raises(subprocess.TimeoutExpired):
        windows_host.boot_archive(
            admitted_windows(root),
            capsule,
            session_root=root / "data/session",
            events=events.append,
        )

    assert events == ["host-preparation"]
