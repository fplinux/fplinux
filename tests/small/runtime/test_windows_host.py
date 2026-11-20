# SPDX-License-Identifier: GPL-2.0-only
"""Test selection from controlled Windows PnP and network snapshots."""

from __future__ import annotations

import copy
import json
import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime.windows import boot as windows_boot
from fplinux_cli.runtime.windows import bundle as windows_bundle
from fplinux_cli.runtime.windows import host as windows_host
from fplinux_cli.runtime.windows import transport as windows_transport
from fplinux_cli.runtime.windows.host import bound_interface, exact_usb_devices

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


@pytest.mark.parametrize("platform", ["ums9117", "sc9820e"])
def test_connection_tools_do_not_require_current_ram_payload(
    tmp_path: Path, platform: str
) -> None:
    root = archive_files(tmp_path, platform=platform)
    (root / "image/ramboot.bin").unlink()
    (root / "build-manifest.json").unlink()
    (root / "runtime-manifest.json").unlink()

    tools = windows_bundle.validate_connection_tools(lambda name: root / name)

    assert tools["ssh"] == root / "tools/openssh/ssh.exe"
    assert tools["keyscan"] == root / "tools/openssh/ssh-keyscan.exe"
    assert "loader" not in tools


@pytest.mark.parametrize("damage", ["changed-tool", "missing-hash", "wrong-path"])
def test_connection_tools_reject_unverified_executables(tmp_path: Path, damage: str) -> None:
    root = archive_files(tmp_path)
    manifest_path = root / "windows-host.json"
    manifest = json.loads(manifest_path.read_text())
    if damage == "changed-tool":
        (root / "tools/openssh/ssh.exe").write_bytes(b"different tool\n")
    elif damage == "missing-hash":
        del manifest["sha256"]["tools/openssh/ssh.exe"]
    else:
        manifest["host_tools"]["ssh"] = "tools/other.exe"
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(SystemExit, match=r"checksum|host tools"):
        windows_bundle.validate_connection_tools(lambda name: root / name)


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
        windows_bundle.validate_archive(
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
    bundle = admitted_windows(archive_files(tmp_path))
    transport = windows_transport.WindowsTransport(host, bundle.tools, wait_seconds=3)

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


def test_enrolled_card_requires_exact_usb_ancestry_with_the_saved_dhcp_binding(
    tmp_path: Path,
) -> None:
    snapshot = endpoint_snapshot()
    session = endpoint_session(tmp_path / "card")
    session.pop("host_mac")
    session["card_access"] = True
    assert bound_interface(snapshot, session) == 17

    snapshot["adapters"][0]["lineage"][-1] = "USB\\VID_0525&PID_A4A6\\another-phone"
    assert bound_interface(snapshot, session) is None


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

    monkeypatch.setattr(windows_boot, "WindowsHost", TimingOutWindowsHost)
    events: list[str] = []

    with pytest.raises(subprocess.TimeoutExpired):
        windows_boot.boot_archive(
            admitted_windows(root),
            capsule,
            log=root / "logs/runtime.log",
            session_root=root / "data/session",
            events=events.append,
        )

    assert events == ["host-preparation"]
