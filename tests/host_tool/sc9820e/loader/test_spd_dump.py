# SPDX-License-Identifier: GPL-2.0-only
"""Run the pinned C uploader with literal USB transfers; no phone is exercised."""

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

FIXTURES = ROOT / "tests/host_tool/sc9820e/loader"
SELECTED = ("--wait", "0", "--usb-bus", "7", "--usb-device", "9")


@pytest.fixture(scope="module")
def uploader(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Keep the complete platform recipe and the literal USB peer in one process."""
    with build_uploader(tmp_path_factory, platform="sc9820e") as executable:
        yield executable


@pytest.fixture
def ram_upload(tmp_path: Path) -> tuple[Path, list[str]]:
    """Use small RAM inputs with escaped bytes in the first FDL."""
    (tmp_path / "fdl1.bin").write_bytes(b"F~}1")
    (tmp_path / "ram.bin").write_bytes(b"RAM!")
    (tmp_path / "fdl2.bin").write_bytes(b"FDL2")
    return tmp_path, (FIXTURES / "ram-upload.plan").read_text().splitlines()


def _arguments(*, final_command: str = "fdl_exec") -> tuple[str, ...]:
    return (
        "keep_charge",
        "1",
        "fdl",
        "fdl1.bin",
        "0x5000",
        "write_data",
        "0x80008000",
        "0",
        "4",
        "ram.bin",
        final_command,
        "fdl2.bin",
        "0x9efffe00",
    )


def _run(  # noqa: PLR0913 -- preserve independent process and USB-peer inputs.
    uploader: Path,
    directory: Path,
    arguments: tuple[str, ...],
    plan: list[str],
    *,
    scenario: str = "normal",
    acknowledgement: str | None = None,
) -> tuple[subprocess.CompletedProcess[str], str]:
    (directory / "usb-peer-scenario").write_text(scenario + "\n")
    (directory / "usb-peer.plan").write_text("\n".join(plan) + ("\n" if plan else ""))
    if acknowledgement is None:
        result = run_process(
            [str(uploader), *arguments],
            name="run source-built RAM uploader with USB peer",
            cwd=directory,
            timeout=5,
        )
    else:
        result = subprocess.run(
            [str(uploader), *arguments],
            cwd=directory,
            input=acknowledgement,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    log = directory / "usb-peer.log"
    return result, log.read_text() if log.exists() else ""


def test_explicit_device_receives_all_ram_frames_and_final_exec_without_a_reply(
    uploader: Path, ram_upload: tuple[Path, list[str]]
) -> None:
    """The other matching device stays unopened while the complete transcript is sent."""
    directory, plan = ram_upload

    result, log = _run(uploader, directory, (*SELECTED, *_arguments()), plan)

    assert result.returncode == 0, result.stderr
    assert "open 7:9\n" in log
    assert "7:8" not in log
    assert "default-open" not in log
    assert log.endswith("out 7:9 7e00040000fffb7e\nclose 7:9\nexit\n")


def test_native_acknowledgement_releases_the_complete_ram_chain_and_final_exec(
    uploader: Path, ram_upload: tuple[Path, list[str]]
) -> None:
    directory, plan = ram_upload

    result, log = _run(
        uploader,
        directory,
        ("--wait", "0", "--ready-ack", *_arguments()),
        plan,
        scenario="discovery-ready",
        acknowledgement="\x06",
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == "FPLINUX_BOOTROM_READY\n"
    assert log.startswith("init\nlist\nopen 7:8\nclaim 7:8 0\ncontrol 7:8\n")
    assert log.endswith("out 7:8 7e00040000fffb7e\nclose 7:8\nexit\n")
    assert "default-open" not in log
    assert "7:9" not in log


@pytest.mark.parametrize(
    "options",
    [
        ("--usb-bus",),
        ("--usb-device",),
        ("--usb-bus", "7"),
        ("--usb-device", "9"),
        ("--usb-bus", "7", "--usb-bus", "7", "--usb-device", "9"),
        ("--usb-bus", "7", "--usb-device", "9", "--usb-device", "9"),
        ("--usb-bus", "", "--usb-device", "9"),
        ("--usb-bus", "+7", "--usb-device", "9"),
        ("--usb-bus", "7tail", "--usb-device", "9"),
        ("--usb-bus", "0", "--usb-device", "9"),
        ("--usb-bus", "256", "--usb-device", "9"),
        ("--usb-bus", "184467440737095516160", "--usb-device", "9"),
        ("--usb-bus", "7", "--usb-device", "0"),
        ("--usb-bus", "7", "--usb-device", "128"),
        ("--usb-bus", "7", "--usb-device", "184467440737095516160"),
    ],
)
def test_invalid_or_incomplete_selector_fails_before_usb_initialization(
    uploader: Path, tmp_path: Path, options: tuple[str, ...]
) -> None:
    result, log = _run(uploader, tmp_path, options, [])

    assert result.returncode == 1, result.stderr
    assert "USB" in result.stderr
    assert log == ""


@pytest.mark.parametrize(
    "scenario",
    ["absent", "ambiguous", "wrong-id", "descriptor-error", "open-error", "list-error"],
)
def test_unavailable_or_mismatched_selected_device_never_uses_the_other_match(
    uploader: Path, tmp_path: Path, scenario: str
) -> None:
    result, log = _run(uploader, tmp_path, SELECTED, [], scenario=scenario)

    assert result.returncode == 1, result.stderr
    assert "default-open" not in log
    assert "7:8" not in log
    assert "control" not in log
    assert "out " not in log
    assert "in " not in log
    if scenario != "open-error":
        assert "open " not in log


@pytest.mark.parametrize(
    ("line", "replacement"),
    [
        pytest.param(3, "in 7e00840000e79a7e", id="boot-connect-bad-ack"),
        pytest.param(11, "timeout-in -", id="first-exec-timeout"),
        pytest.param(20, "short-out 7e0002000452414d2160977e", id="ram-short-write"),
        pytest.param(23, "in 7e00840000ff7b7e", id="ram-end-bad-ack"),
        pytest.param(29, "in 7e00840000ff7b7e", id="final-end-bad-ack"),
        pytest.param(29, "timeout-in -", id="final-end-timeout"),
        pytest.param(29, "disconnect-in -", id="final-end-disconnect"),
        pytest.param(30, "short-out 7e00040000fffb7e", id="final-exec-short-write"),
        pytest.param(30, "error-out 7e00040000fffb7e", id="final-exec-transfer-error"),
    ],
)
def test_transfer_failure_before_complete_final_exec_remains_an_error(
    uploader: Path,
    ram_upload: tuple[Path, list[str]],
    line: int,
    replacement: str,
) -> None:
    directory, plan = ram_upload
    plan[line] = replacement

    result, log = _run(uploader, directory, (*SELECTED, *_arguments()), plan)

    assert result.returncode == 1, result.stderr
    assert "USB peer:" not in result.stderr
    assert "exit\n" not in log
    if line < 30:
        assert "out 7:9 7e00040000fffb7e\n" not in log


@pytest.mark.parametrize(
    ("reply", "status", "message"),
    [
        ("in 7e00800000ff7f7e", 0, ""),
        ("in 7e00840000ff7b7e", 1, "unexpected response"),
        ("timeout-in -", 1, "timeout reached"),
        ("disconnect-in -", 1, "connection closed"),
    ],
)
def test_ordinary_fdl_keeps_its_final_ack_requirement_and_unique_usb_discovery(
    uploader: Path,
    ram_upload: tuple[Path, list[str]],
    reply: str,
    status: int,
    message: str,
) -> None:
    directory, plan = ram_upload
    plan.append(reply)

    result, log = _run(
        uploader,
        directory,
        ("--wait", "0", *_arguments(final_command="fdl")),
        plan,
        scenario="discovery-ready",
    )

    assert result.returncode == status, result.stderr
    assert message in result.stderr
    assert "list\nopen 7:8\n" in log
    assert "default-open" not in log
    assert "7:9" not in log


@pytest.mark.parametrize(
    "tail",
    [
        ("fdl_exec", "fdl2.bin", "0x9efffe00", "reset"),
        ("end_data", "0", "fdl_exec", "fdl2.bin", "0x9efffe00"),
    ],
    ids=["following-command", "missing-end-ack"],
)
def test_final_exec_requires_final_position_and_acknowledged_end_data(
    uploader: Path, ram_upload: tuple[Path, list[str]], tail: tuple[str, ...]
) -> None:
    directory, plan = ram_upload
    first_fdl = ("keep_charge", "1", "fdl", "fdl1.bin", "0x5000")

    result, log = _run(uploader, directory, (*SELECTED, *first_fdl, *tail), plan[:18])

    assert result.returncode == 1, result.stderr
    assert "fdl_exec requires" in result.stderr
    assert "out 7:9 7e000100089efffe000000000462f27e\n" not in log


def test_final_exec_cannot_replace_the_first_fdl_handshake(
    uploader: Path, ram_upload: tuple[Path, list[str]]
) -> None:
    directory, _plan = ram_upload

    result, log = _run(
        uploader,
        directory,
        (*SELECTED, "fdl_exec", "fdl2.bin", "0x9efffe00"),
        [],
    )

    assert result.returncode == 1, result.stderr
    assert "fdl_exec requires" in result.stderr
    assert "control" not in log
    assert "out " not in log
