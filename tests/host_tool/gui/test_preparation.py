# SPDX-License-Identifier: GPL-2.0-only
"""Production preparation status and display with a controlled DRM boundary."""

from __future__ import annotations

import json
import os
import select
import shlex
import signal
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.display.status_resources import generate
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-session"
PEERS = ROOT / "tests/host_tool/gui"


@dataclass(frozen=True)
class PreparationBinaries:
    """The display process and its independent status publisher."""

    command: Path
    status: Path
    renderer: Path


@pytest.fixture(scope="module")
def preparation(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[PreparationBinaries]:
    with c_build_directory(tmp_path_factory, "preparation") as directory:
        fonts = generate(directory, ui_host_inputs)
        command = directory / "preparation"
        status = directory / "status"
        renderer = directory / "preparation-render"
        json_flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "--libs", "json-c"],
                name="read preparation JSON flags",
                timeout=10,
                check=True,
            ).stdout
        )
        drm_flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read preparation DRM flags",
                timeout=10,
                check=True,
            ).stdout
        )
        flags = [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{directory}",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{APORT}",
        ]
        compile_c(
            command,
            [
                *flags,
                *drm_flags,
                str(APORT / "preparation.c"),
                str(APORT / "preparation-status.c"),
                str(APORT / "preparation-render.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-activity.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                str(fonts),
                str(PEERS / "preparation-session.c"),
                *json_flags,
            ],
            name="compile preparation with simulated DRM",
        )
        compile_c(
            status,
            [
                *flags,
                str(PEERS / "preparation-status-test.c"),
                str(APORT / "preparation-status.c"),
                *json_flags,
            ],
            name="compile preparation status reader",
        )
        compile_c(
            renderer,
            [
                *flags,
                str(PEERS / "preparation-render-test.c"),
                str(APORT / "preparation-render.c"),
                str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-activity.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                str(fonts),
            ],
            name="compile preparation state renderer",
        )
        yield PreparationBinaries(command, status, renderer)


@pytest.mark.parametrize("profile", ["128x128", "128x160", "176x220", "240x320", "320x240"])
@pytest.mark.parametrize(
    "state",
    [
        "connecting",
        "reading-zero",
        "reading-half",
        "reading-full",
        "extracting",
        "ready",
        "cancelled",
        "error",
    ],
)
def test_actual_renderer_displays_state_and_exact_read_progress(
    preparation: PreparationBinaries, profile: str, state: str
) -> None:
    width, height = profile.split("x")
    run_process(
        [str(preparation.renderer), width, height, state],
        name=f"render actual preparation {state} on {profile}",
        timeout=5,
        check=True,
    )


def update(preparation: PreparationBinaries, status: Path, stage: str, *counts: str) -> None:
    result = run_process(
        [str(preparation.command), "--stage", stage, *counts, str(status)],
        name=f"write preparation stage {stage}",
        timeout=5,
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("stage", ["waiting", "extracting", "ready", "error", "cancelled"])
def test_stage_writer_needs_no_display(
    preparation: PreparationBinaries, tmp_path: Path, stage: str
) -> None:
    status = tmp_path / "status.json"
    update(preparation, status, stage)
    assert json.loads(status.read_bytes()) == {"stage": stage}
    assert list(tmp_path.iterdir()) == [status]


@pytest.mark.parametrize(
    ("completed", "total"),
    [(0, 67108864), (33554432, 67108864), (67108864, 67108864), (2**64 - 1, 2**64 - 1)],
)
def test_reading_preserves_exact_byte_counts(
    preparation: PreparationBinaries, tmp_path: Path, completed: int, total: int
) -> None:
    status = tmp_path / "status.json"
    update(preparation, status, "reading", "--completed", str(completed), "--total", str(total))
    assert json.loads(status.read_bytes()) == {
        "stage": "reading",
        "completed": completed,
        "total": total,
    }
    result = run_process(
        [str(preparation.status), str(status)],
        name="read exact byte counts",
        timeout=5,
        check=True,
    )
    assert result.stdout == f"valid 1 {completed} {total}\n"


@pytest.mark.parametrize(
    "arguments",
    [
        ["--stage", "unknown"],
        ["--stage", "reading"],
        ["--stage", "reading", "--completed", "1", "--total", "0"],
        ["--stage", "reading", "--completed", "2", "--total", "1"],
        ["--stage", "reading", "--completed", "-1", "--total", "2"],
        ["--stage", "reading", "--completed", "+1", "--total", "2"],
        ["--stage", "reading", "--completed", "0", "--total", "18446744073709551616"],
        ["--stage", "ready", "--completed", "1", "--total", "2"],
        ["--completed", "1", "--total", "2"],
    ],
)
def test_invalid_updates_preserve_previous_status(
    preparation: PreparationBinaries, tmp_path: Path, arguments: list[str]
) -> None:
    status = tmp_path / "status.json"
    previous = b'{"stage":"waiting"}\n'
    status.write_bytes(previous)
    result = run_process(
        [str(preparation.command), *arguments, str(status)],
        name="reject invalid preparation values",
        timeout=5,
    )
    assert result.returncode == 2
    assert status.read_bytes() == previous


@pytest.mark.parametrize(
    "payload",
    [
        b'{"stage":',
        b'{"stage":"other"}',
        b'{"stage":"ready"} false',
        b'{"stage":"reading","completed":-1,"total":2}',
        b'{"stage":"reading","completed":0,"total":0}',
        b'{"stage":"reading","completed":3,"total":2}',
        b'{"stage":"reading","completed":0.5,"total":2}',
        b'{"stage":"reading","total":2}',
        b'{"stage":"ready","completed":1}',
        b'{"stage":"ready\\u0000reading"}',
        b" " * 512,
    ],
)
def test_malformed_status_preserves_state(
    preparation: PreparationBinaries, tmp_path: Path, payload: bytes
) -> None:
    status = tmp_path / "status.json"
    status.write_bytes(payload)
    result = run_process(
        [str(preparation.status), str(status)], name="read malformed status", timeout=5, check=True
    )
    assert result.stdout == "invalid 3 41 42\n"


def test_help_precedes_invalid_values_and_resource_access(
    preparation: PreparationBinaries,
) -> None:
    result = run_process(
        [str(preparation.command), "--stage", "invalid", "--help"],
        name="show preparation help without display access",
        timeout=5,
    )
    assert result.returncode == 0
    assert "STATUS_FILE" in result.stdout
    assert "--completed=BYTES" in result.stdout
    assert result.stderr == ""


def test_failed_publish_removes_temporary_file(
    preparation: PreparationBinaries, tmp_path: Path
) -> None:
    status = tmp_path / "status.json"
    status.mkdir()
    result = run_process(
        [str(preparation.command), "--stage", "ready", str(status)],
        name="fail status publication at a directory",
        timeout=5,
    )
    assert result.returncode == 1
    assert "cannot write status" in result.stderr
    assert list(tmp_path.iterdir()) == [status]


def test_missing_status_means_waiting(preparation: PreparationBinaries, tmp_path: Path) -> None:
    result = run_process(
        [str(preparation.status), str(tmp_path / "missing")],
        name="read absent preparation status",
        timeout=5,
        check=True,
    )
    assert result.stdout == "valid 0 0 0\n"


def next_event(process: subprocess.Popen[str], deadline: float) -> str:
    assert process.stdout is not None
    value = bytearray()
    while True:
        readable, _, _ = select.select(
            [process.stdout.fileno()], [], [], max(0, deadline - time.monotonic())
        )
        assert readable, "preparation did not report its next event"
        byte = os.read(process.stdout.fileno(), 1)
        assert byte
        if byte == b"\n":
            return value.decode("ascii")
        value.extend(byte)


def next_frame(process: subprocess.Popen[str], deadline: float, path: Path) -> bytes:
    assert next_event(process, deadline).startswith("frame ")
    return path.read_bytes()


def terminal_frame(process: subprocess.Popen[str], deadline: float, path: Path) -> bytes:
    assert process.stdout is not None
    image = next_frame(process, deadline, path)
    while select.select([process.stdout.fileno()], [], [], 0.3)[0]:
        assert time.monotonic() < deadline
        image = next_frame(process, deadline, path)
    return image


@pytest.mark.parametrize(
    ("width", "height"), [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)]
)
def test_busy_reading_terminal_and_vt(
    preparation: PreparationBinaries, tmp_path: Path, width: int, height: int
) -> None:
    status = tmp_path / "status.json"
    output = tmp_path / "frame"
    vt = tmp_path / "vt"
    os.mkfifo(vt)
    environment = os.environ | {
        "FPLINUX_TEST_GEOMETRY": f"{width}x{height}",
        "FPLINUX_TEST_VT": str(vt),
        "FPLINUX_TEST_FRAME": str(output),
    }

    def inspect(process: subprocess.Popen[str], deadline: float) -> None:
        assert next_event(process, deadline) == "opened"
        busy = next_frame(process, deadline, output)
        assert next_frame(process, deadline, output) != busy
        update(preparation, status, "reading", "--completed", "33554432", "--total", "67108864")
        reading = terminal_frame(process, deadline, output)
        assert reading != busy
        for stage in ("ready", "error", "cancelled"):
            update(preparation, status, stage)
            terminal = terminal_frame(process, deadline, output)
            assert terminal != reading
        with vt.open("wb", buffering=0) as channel:
            channel.write(b"0")
            assert next_event(process, deadline) == "inactive"
            channel.write(b"1")
            assert next_event(process, deadline) == "active"
            assert next_frame(process, deadline, output) == terminal
        process.send_signal(signal.SIGTERM)

    result = run_process(
        [str(preparation.command), str(status)],
        name="observe preparation display",
        timeout=10,
        env=environment,
        while_running=inspect,
    )
    assert result.returncode == 143, result.stderr
    assert result.stdout == "closed\n"
