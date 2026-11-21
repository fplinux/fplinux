# SPDX-License-Identifier: GPL-2.0-only
"""Session station requests against an isolated supplicant D-Bus peer."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def run_station_peer(tmp_path: Path, scenario: str) -> Path:
    executable = tmp_path / "wifi-station-peer"
    config_directory = tmp_path / "wifi"
    config_directory.mkdir(mode=0o700)
    config = config_directory / "wpa_supplicant.conf"
    processes = tmp_path / "processes"
    processes.mkdir()
    session = ROOT / "alpine/aports/fplinux-session"
    dbus = run_process(
        ["pkg-config", "--cflags", "--libs", "dbus-1"],
        name="resolve the station D-Bus dependency",
        timeout=10,
        check=True,
    ).stdout.split()
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{session}",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/session/wifi-station-peer.c"),
            str(session / "session-wifi-station.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-Wl,--wrap=opendir",
            "-Wl,--wrap=stat",
            *dbus,
        ],
        name="compile the station D-Bus peer",
    )
    run_process(
        ["dbus-run-session", "--", str(executable), str(config), str(processes), scenario],
        name=f"check station {scenario}",
        timeout=15,
        check=True,
    )
    return config


def test_cancelled_wifi_request_cannot_publish_a_profile(tmp_path: Path) -> None:
    published = run_station_peer(tmp_path, "cancel").read_bytes()
    assert b'\tpsk="accepted-pass"\n' in published
    assert b"rejected-pass" not in published
    assert published.count(b"network={\n") == 1
    assert b"\tdisabled=1\n" in published


def test_station_can_claim_unmanaged_wifi_without_network_namespaces(tmp_path: Path) -> None:
    run_station_peer(tmp_path, "no-netns")
