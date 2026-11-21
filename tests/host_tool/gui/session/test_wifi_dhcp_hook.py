# SPDX-License-Identifier: GPL-2.0-only
"""Wi-Fi DHCP lease events with a controlled ip command."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

HOOK = ROOT / "alpine/aports/fplinux-session/wifi-udhcpc-hook"


@dataclass(frozen=True)
class DhcpHook:
    """An isolated lease directory and address/route command trace."""

    directory: Path
    trace: Path
    environment: dict[str, str]

    def event(self, name: str, **options: str) -> None:
        """Deliver one udhcpc event with its exported lease options."""
        run_process(
            ["/bin/sh", str(HOOK), name],
            name=f"run Wi-Fi DHCP {name} event",
            timeout=5,
            env={**self.environment, **options},
            check=True,
        )


@pytest.fixture
def dhcp_hook(tmp_path: Path) -> DhcpHook:
    directory = tmp_path / "lease"
    directory.mkdir()
    trace = tmp_path / "ip-trace"
    ip_peer = tmp_path / "ip"
    shutil.copyfile(ROOT / "tests/host_tool/gui/session/wifi-dhcp-ip.sh", ip_peer)
    ip_peer.chmod(0o755)
    return DhcpHook(
        directory,
        trace,
        {
            "FPLINUX_WIFI_DHCP_STATE": str(directory),
            "IP_TRACE": str(trace),
            "PATH": f"{tmp_path}:/usr/bin:/bin",
            "interface": "wlan-test",
        },
    )


def test_dhcp_renews_then_removes_only_wifi_lease(dhcp_hook: DhcpHook) -> None:
    dhcp_hook.event("bound", ip="192.0.2.70", mask="24", router="192.0.2.1")
    assert (dhcp_hook.directory / "status").read_text() == "192.0.2.70\n"

    dhcp_hook.event("renew", ip="192.0.2.70", mask="24", router="192.0.2.2")
    assert (dhcp_hook.directory / "status").read_text() == "192.0.2.70\n"

    dhcp_hook.event("deconfig")
    assert not list(dhcp_hook.directory.iterdir())
    assert dhcp_hook.trace.read_text().splitlines() == [
        "-4 addr add 192.0.2.70/24 broadcast + dev wlan-test",
        "-4 route add default via 192.0.2.1 dev wlan-test metric 600 protocol 16 onlink",
        "-4 route del default via 192.0.2.1 dev wlan-test metric 600 protocol 16",
        "-4 route add default via 192.0.2.2 dev wlan-test metric 600 protocol 16 onlink",
        "-4 route del default via 192.0.2.2 dev wlan-test metric 600 protocol 16",
        "-4 addr del 192.0.2.70/24 dev wlan-test",
    ]


def test_dhcp_route_conflict_releases_its_address_and_reports_failure(
    dhcp_hook: DhcpHook,
) -> None:
    dhcp_hook.event("bound", ip="192.0.2.70", mask="24", router="192.0.2.1", IP_REJECT_ROUTE="yes")
    assert (dhcp_hook.directory / "status").read_text() == "failed\n"
    assert dhcp_hook.trace.read_text().splitlines() == [
        "-4 addr add 192.0.2.70/24 broadcast + dev wlan-test",
        "-4 route add default via 192.0.2.1 dev wlan-test metric 600 protocol 16 onlink",
        "-4 addr del 192.0.2.70/24 dev wlan-test",
    ]
