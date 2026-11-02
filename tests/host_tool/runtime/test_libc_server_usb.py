# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the patched bridge with a deterministic USB peer."""

from __future__ import annotations

import os
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from subprocess import CompletedProcess

ARCHIVE = Path("/opt/quality/src/fpdoom-host.tar.gz")
PATCH = ROOT / "platforms/ums9117/host/0001-libc-server-fplinux-handoff.patch"
PEER = ROOT / "tests/host_tool/runtime/libc-server-usb-peer.c"


@pytest.fixture(scope="module")
def bridge_executable(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Apply the production patch and link the actual bridge with the USB peer."""
    with c_build_directory(tmp_path_factory, "libc-server-usb") as directory:
        with tarfile.open(ARCHIVE, "r:gz") as archive:
            for name in ("Makefile", "libc_server.c", "cmd_def.h"):
                members = [
                    m for m in archive.getmembers() if m.name.endswith(f"/libc_server/{name}")
                ]
                assert len(members) == 1
                source = archive.extractfile(members[0])
                assert source is not None
                with source:
                    (directory / name).write_bytes(source.read())
        run_process(
            ["patch", "--batch", "--fuzz=0", "-p1", "-i", str(PATCH)],
            name="apply bridge patch",
            cwd=directory,
            timeout=10,
            check=True,
        )
        executable = directory / "libc-server-usb"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-DUSE_LIBUSB=1",
                f"-I{ROOT / 'platforms/ums9117/bootstrap/src'}",
                str(directory / "libc_server.c"),
                str(PEER),
            ],
            name="compile patched bridge with USB peer",
            check=True,
        )
        yield executable


def run_bridge(executable: Path, scenario: str, wait: int = 2) -> CompletedProcess[str]:
    """Run one bridge process against a selected peer response."""
    environment = os.environ.copy()
    environment["BRIDGE_USB_SCENARIO"] = scenario
    return run_process(
        [str(executable), "--wait", str(wait), "--fplinux-handoff", "0" * 64],
        name=f"bridge USB scenario {scenario}",
        env=environment,
        timeout=10,
    )


def peer_report(result: CompletedProcess[str]) -> dict[str, int]:
    """Read the peer's resource and protocol observations."""
    reports = [line for line in result.stderr.splitlines() if line.startswith("peer ")]
    assert len(reports) == 1, result.stderr
    return {
        key: int(value) for key, value in (field.split("=") for field in reports[0].split()[1:])
    }


@pytest.mark.parametrize(
    ("scenario", "not_ready"),
    [("open-missing", 2), ("descriptor-missing", 2), ("claim-gone", 1)],
)
def test_transient_usb_readiness_reopens_and_completes_handoff(
    bridge_executable: Path, scenario: str, not_ready: int
) -> None:
    result = run_bridge(bridge_executable, scenario)
    assert result.returncode == 0, result.stderr
    assert "!!! x" in result.stdout
    peer = peer_report(result)
    assert peer["not_ready"] == not_ready
    assert peer["acknowledged"] == 1
    assert peer["opens"] == peer["closes"]
    assert peer["allocated"] == peer["freed"]
    assert peer["lifetime_errors"] == 0


@pytest.mark.parametrize("wait", [0, 2])
@pytest.mark.parametrize(
    ("scenario", "error"),
    [("descriptor-gone", "LIBUSB_ERROR_NO_DEVICE"), ("open-gone", "libusb_open_device failed")],
)
def test_disappearing_device_stops_when_wait_expires(
    bridge_executable: Path, wait: int, scenario: str, error: str
) -> None:
    result = run_bridge(bridge_executable, scenario, wait)
    assert result.returncode != 0
    assert error in result.stderr
    peer = peer_report(result)
    assert peer["transfers"] == 0
    assert peer["acknowledged"] == 0
    assert peer["opens"] == peer["closes"]
    assert peer["allocated"] == peer["freed"]
    assert peer["lifetime_errors"] == 0
    if wait == 0:
        assert peer["not_ready"] == 1
        assert peer["waited_us"] == 0
    else:
        assert peer["not_ready"] > 1
        assert 0 < peer["waited_us"] <= 3_000_000


@pytest.mark.parametrize(
    ("scenario", "error"),
    [
        ("descriptor-access", "LIBUSB_ERROR_ACCESS"),
        ("claim-access", "LIBUSB_ERROR_ACCESS"),
        ("descriptor-invalid", "LIBUSB_ERROR_INVALID_PARAM"),
    ],
)
def test_nontransient_usb_error_fails_immediately(
    bridge_executable: Path, scenario: str, error: str
) -> None:
    result = run_bridge(bridge_executable, scenario)
    assert result.returncode != 0
    assert error in result.stderr
    peer = peer_report(result)
    assert peer["opens"] == 1
    assert peer["transfers"] == 0
    assert peer["acknowledged"] == 0
    assert peer["waited_us"] == 0


def test_missing_endpoint_is_not_retried(bridge_executable: Path) -> None:
    result = run_bridge(bridge_executable, "missing-endpoint")
    assert result.returncode != 0
    assert "endp_out not found" in result.stderr
    peer = peer_report(result)
    assert peer["opens"] == 1
    assert peer["transfers"] == 0
    assert peer["acknowledged"] == 0
    assert peer["waited_us"] == 0
