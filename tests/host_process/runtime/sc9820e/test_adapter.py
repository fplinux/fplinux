# SPDX-License-Identifier: GPL-2.0-only
"""Real uploader process lifecycle through the standalone SC9820E adapter."""

from __future__ import annotations

import json
import os
import signal
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

import pytest

from tests import ROOT
from tests.process import process_state, python_environment, run_process

if TYPE_CHECKING:
    import subprocess


def _invoke(root: Path, mode: str) -> subprocess.CompletedProcess[str]:
    def cancel_after_upload_starts(process: subprocess.Popen[str], deadline: float) -> None:
        while not (root / "uploader.json").exists():
            if process.poll() is not None or time.monotonic() >= deadline:
                return
            time.sleep(0.01)
        os.kill(process.pid, signal.SIGTERM)

    return run_process(
        [sys.executable, "-m", "tests.host_process.runtime.sc9820e.runner_peer", str(root), mode],
        name="SC9820E standalone adapter",
        timeout=10,
        cwd=ROOT,
        env=python_environment(),
        while_running=cancel_after_upload_starts if mode == "cancel" else None,
    )


def _receipt(root: Path) -> dict[str, Any]:
    return cast("dict[str, Any]", json.loads((root / "uploader.json").read_text(encoding="utf-8")))


@pytest.mark.parametrize(
    ("mode", "trace_arguments"),
    [("ready", []), ("trace", ["--verbose", "1"])],
    ids=["default", "loader-trace"],
)
def test_adapter_observes_exact_fixed_command_and_requires_kernel_identity(
    tmp_path: Path, mode: str, trace_arguments: list[str]
) -> None:
    result = _invoke(tmp_path, mode)
    assert result.returncode == 0, result.stderr
    receipt = _receipt(tmp_path)
    directory = Path(receipt["directory"])
    assert receipt["arguments"] == [
        *trace_arguments,
        "--wait",
        "0",
        "--usb-bus",
        "3",
        "--usb-device",
        "41",
        "keep_charge",
        "1",
        "fdl",
        str(directory / "fdl1.bin"),
        "0x5000",
        "write_data",
        "0x8e000000",
        "0",
        "14",
        str(directory / "sml.bin"),
        "write_data",
        "0x81008000",
        "0",
        "9",
        str(directory / "kernel.bin"),
        "write_data",
        "0x85400000",
        "0",
        str(len(bytes.fromhex(receipt["files"]["dtb"]["bytes"]))),
        str(directory / "dtb.bin"),
        "write_data",
        "0x85500000",
        "0",
        "7",
        str(directory / "initramfs.bin"),
        "write_data",
        "0x80002060",
        "0",
        "8",
        str(directory / "entry.bin"),
        "timeout",
        "60000",
        "fdl_exec",
        str(directory / "fdl2.bin"),
        "0x9efffe00",
    ]
    assert all(record["mode"] == 0o600 for record in receipt["files"].values())
    assert not directory.exists()
    assert not list((tmp_path / "runtime/sessions").glob("*/ramboot.bin"))
    assert (tmp_path / "events").read_text(encoding="ascii").splitlines() == [
        "waiting-for-device",
        "device-found",
        "ram-loader-start",
        "ram-loader-complete",
        "linux-start",
        "linux-transition",
        "linux-usb",
        "ssh-ready",
        "complete",
    ]
    commands = (tmp_path / "ssh-commands").read_text(encoding="utf-8").splitlines()
    assert commands == ["uname -r"]


@pytest.mark.parametrize(
    ("mode", "message"),
    [
        ("wrong-identity", "different kernel identity"),
        ("wrong-connection", "different or ambiguous physical connection"),
        ("loader-failure", "exit status 17"),
    ],
)
def test_adapter_never_reports_ready_after_a_failed_transfer_or_identity(
    tmp_path: Path, mode: str, message: str
) -> None:
    result = _invoke(tmp_path, mode)
    assert result.returncode != 0
    assert message in result.stderr
    events = (tmp_path / "events").read_text(encoding="ascii").splitlines()
    assert "ssh-ready" not in events
    assert "complete" not in events
    assert not Path(_receipt(tmp_path)["directory"]).exists()
    assert not list((tmp_path / "runtime/sessions").glob("*/ramboot.bin"))


def test_adapter_rejects_ambiguous_bootrom_without_spawning_the_uploader(tmp_path: Path) -> None:
    result = _invoke(tmp_path, "ambiguous")
    assert result.returncode != 0
    assert "more than one BootROM" in result.stderr
    assert not (tmp_path / "uploader.json").exists()
    assert not list((tmp_path / "runtime/sessions").glob("*/ramboot.bin"))


def test_adapter_reaps_the_real_uploader_and_removes_private_images_on_system_exit(
    tmp_path: Path,
) -> None:
    result = _invoke(tmp_path, "cancel")
    assert result.returncode == 143, result.stderr
    receipt = _receipt(tmp_path)
    assert process_state(receipt["pid"]) is None
    assert not Path(receipt["directory"]).exists()
    assert not list((tmp_path / "runtime/sessions").glob("*/ramboot.bin"))
    assert "ram-loader-complete" not in (tmp_path / "events").read_text(encoding="ascii")
