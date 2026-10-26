# SPDX-License-Identifier: GPL-2.0-only
"""Run native USB discovery through the complete source-built RAM uploader."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process
from tests.spd_dump_support import build_uploader

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/ums9117/loader"


@pytest.fixture(scope="module", params=["ums9117", "sc9820e"])
def uploader(
    request: pytest.FixtureRequest, tmp_path_factory: pytest.TempPathFactory
) -> Iterator[Path]:
    """Exercise both current consumers of shared discovery and readiness behavior."""
    with build_uploader(tmp_path_factory, platform=request.param) as executable:
        yield executable


@pytest.fixture
def fdl_upload(tmp_path: Path) -> tuple[Path, list[str]]:
    """Keep the first FDL's literal RAM bytes and expected wire frames independent."""
    (tmp_path / "fdl1.bin").write_bytes(b"F~}1")
    return tmp_path, (FIXTURES / "fdl-upload.plan").read_text().splitlines()


def _run(  # noqa: PLR0913 -- preserve independent process and USB-peer inputs.
    uploader: Path,
    directory: Path,
    *,
    scenario: str,
    wait_seconds: int = 20,
    arguments: tuple[str, ...] = (),
    plan: list[str] | None = None,
    acknowledgement: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], str]:
    (directory / "usb-peer-scenario").write_text(scenario + "\n")
    frames = plan if plan is not None else []
    (directory / "usb-peer.plan").write_text("\n".join(frames) + ("\n" if frames else ""))
    command = [str(uploader), "--wait", str(wait_seconds), *arguments]
    if acknowledgement is None:
        result = run_process(
            command,
            name="run source-built uploader with USB peer",
            cwd=directory,
            timeout=5,
        )
    else:
        result = subprocess.run(
            command,
            cwd=directory,
            input=acknowledgement,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    log = directory / "usb-peer.log"
    return result, log.read_text() if log.exists() else ""


@pytest.mark.parametrize(
    ("scenario", "wait_seconds", "scans", "address"),
    [
        ("discovery-ready", 0, 1, "7:8"),
        ("discovery-delayed", 1, 2, "7:8"),
        ("discovery-unrelated-first", 0, 1, "7:9"),
    ],
)
# Keep fixtures and independent discovery expectations visible in the parameterized case.
def test_present_or_delayed_native_device_receives_one_fdl_upload(  # noqa: PLR0913, PLR0917
    uploader: Path,
    fdl_upload: tuple[Path, list[str]],
    scenario: str,
    wait_seconds: int,
    scans: int,
    address: str,
) -> None:
    directory, plan = fdl_upload

    result, log = _run(
        uploader,
        directory,
        scenario=scenario,
        wait_seconds=wait_seconds,
        arguments=("fdl", "fdl1.bin", "0x5000"),
        plan=plan,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert log.splitlines().count("list") == scans
    assert log.splitlines().count(f"open {address}") == 1
    assert "default-open" not in log
    assert f"out {address} 7e00020004467d5e7d5d313b9e7e\n" in log
    assert log.endswith(f"close {address}\nexit\n")


def test_native_readiness_without_commands_opens_claims_and_closes_without_transfers(
    uploader: Path, tmp_path: Path
) -> None:
    result, log = _run(uploader, tmp_path, scenario="discovery-ready", wait_seconds=0)

    assert result.returncode == 0, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert log == "init\nlist\nopen 7:8\nclaim 7:8 0\nclose 7:8\nexit\n"


@pytest.mark.parametrize("acknowledgement", ["", "\x00"])
def test_native_ack_barrier_rejects_eof_or_another_byte_before_any_transfer(
    uploader: Path, tmp_path: Path, acknowledgement: str
) -> None:
    result, log = _run(
        uploader,
        tmp_path,
        scenario="discovery-ready",
        wait_seconds=0,
        arguments=("--ready-ack", "fdl", "unused.bin", "0x5000"),
        acknowledgement=acknowledgement,
    )

    assert result.returncode == 1, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert "BootROM transfer was not acknowledged" in result.stderr
    assert log == "init\nlist\nopen 7:8\nclaim 7:8 0\nclose 7:8\nexit\n"


def test_native_ack_byte_allows_the_literal_fdl_upload(
    uploader: Path, fdl_upload: tuple[Path, list[str]]
) -> None:
    directory, plan = fdl_upload
    result, log = _run(
        uploader,
        directory,
        scenario="discovery-ready",
        wait_seconds=0,
        arguments=("--ready-ack", "fdl", "fdl1.bin", "0x5000"),
        plan=plan,
        acknowledgement="\x06",
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert "out 7:8 7e00020004467d5e7d5d313b9e7e\n" in log
    assert log.endswith("close 7:8\nexit\n")


@pytest.mark.parametrize("wait_seconds", [0, 1])
def test_absent_device_stops_within_the_selected_native_wait(
    uploader: Path, tmp_path: Path, wait_seconds: int
) -> None:
    result, log = _run(uploader, tmp_path, scenario="discovery-absent", wait_seconds=wait_seconds)

    assert result.returncode == 1, result.stderr
    assert result.stdout == ""
    assert "BootROM USB device not found" in result.stderr
    scans = log.splitlines().count("list")
    if wait_seconds:
        assert scans > 1
    else:
        assert scans == 1
    assert "open " not in log
    assert "control " not in log


def test_other_usb_id_is_not_opened_as_bootrom(uploader: Path, tmp_path: Path) -> None:
    result, log = _run(uploader, tmp_path, scenario="discovery-other-id", wait_seconds=0)

    assert result.returncode == 1, result.stderr
    assert result.stdout == ""
    assert "BootROM USB device not found" in result.stderr
    assert log.splitlines().count("list") == 1
    assert "open " not in log


@pytest.mark.parametrize(
    ("scenario", "message", "opens"),
    [
        ("list-error", "libusb_get_device_list failed: LIBUSB_ERROR_IO", 0),
        (
            "discovery-descriptor-error",
            "libusb_get_device_descriptor failed: LIBUSB_ERROR_IO",
            0,
        ),
        ("discovery-open-access", "libusb_open BootROM device failed: LIBUSB_ERROR_ACCESS", 1),
        ("discovery-open-busy", "libusb_open BootROM device failed: LIBUSB_ERROR_BUSY", 1),
        (
            "discovery-open-disconnected",
            "libusb_open BootROM device failed: LIBUSB_ERROR_NO_DEVICE",
            1,
        ),
        ("discovery-claim-access", "libusb_claim_interface failed : LIBUSB_ERROR_ACCESS", 1),
        ("discovery-claim-busy", "libusb_claim_interface failed : LIBUSB_ERROR_BUSY", 1),
        (
            "discovery-claim-disconnected",
            "libusb_claim_interface failed : LIBUSB_ERROR_NO_DEVICE",
            1,
        ),
        ("discovery-ambiguous", "multiple BootROM USB devices are connected", 0),
    ],
)
def test_discovery_open_or_claim_error_fails_without_waiting_or_starting_fdl(
    uploader: Path, tmp_path: Path, scenario: str, message: str, opens: int
) -> None:
    result, log = _run(uploader, tmp_path, scenario=scenario)

    assert result.returncode == 1, result.stderr
    assert result.stdout == ""
    assert message in result.stderr
    assert log.splitlines().count("list") == 1
    assert log.splitlines().count("open 7:8") == opens
    assert "control " not in log
    assert "out " not in log
    assert "in " not in log


@pytest.mark.parametrize(
    ("scenario", "message", "claims"),
    [
        (
            "discovery-open-driver-needed",
            "libusb_open BootROM device failed: LIBUSB_ERROR_NOT_SUPPORTED",
            0,
        ),
        (
            "discovery-claim-driver-needed",
            "libusb_claim_interface failed : LIBUSB_ERROR_NOT_SUPPORTED",
            1,
        ),
    ],
)
def test_unsupported_native_open_or_claim_reports_driver_needed_before_transfers(
    uploader: Path, tmp_path: Path, scenario: str, message: str, claims: int
) -> None:
    result, log = _run(uploader, tmp_path, scenario=scenario)

    assert result.returncode == 2, result.stderr
    assert result.stdout == ""
    assert message in result.stderr
    assert log.splitlines().count("list") == 1
    assert log.splitlines().count("open 7:8") == 1
    assert log.splitlines().count("claim 7:8 0") == claims
    assert "control " not in log
    assert "out " not in log
    assert "in " not in log


@pytest.mark.parametrize(
    ("operation", "message"),
    [("error-out", "LIBUSB_ERROR_IO"), ("unsupported-out", "LIBUSB_ERROR_NOT_SUPPORTED")],
)
def test_partial_fdl_failure_does_not_repeat_native_discovery_or_open(
    uploader: Path, fdl_upload: tuple[Path, list[str]], operation: str, message: str
) -> None:
    directory, plan = fdl_upload
    plan = [*plan[:6], f"{operation} 7e00020004467d5e7d5d313b9e7e"]

    result, log = _run(
        uploader,
        directory,
        scenario="discovery-ready",
        arguments=("fdl", "fdl1.bin", "0x5000"),
        plan=plan,
    )

    assert result.returncode == 1, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert f"usb_send failed : {message}" in result.stderr
    assert log.splitlines().count("list") == 1
    assert log.splitlines().count("open 7:8") == 1
    assert f"{operation} 7:8 7e00020004467d5e7d5d313b9e7e\n" in log
    assert "exit\n" not in log
