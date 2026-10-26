# SPDX-License-Identifier: GPL-2.0-only
"""Real status-file processes and rendering with a controlled DRM/VT boundary."""

from __future__ import annotations

import json
import os
import select
import shlex
import signal
import struct
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

import fplinux_ui_profile
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-session"
PEERS = ROOT / "tests/host_tool/gui"


@dataclass(frozen=True)
class PreparationBinaries:
    """Production commands linked to real file/font code and simulated DRM."""

    command: Path
    status: Path
    small_font: Path
    large_font: Path


@pytest.fixture(scope="module")
def preparation_profile(request: pytest.FixtureRequest, palette_preparation_profile: Path) -> Path:
    if getattr(request, "param", None) == "variant":
        return palette_preparation_profile
    return ROOT / "lib/fplinux/fplinux-ui-profile.c"


@pytest.fixture(scope="module")
def palette_preparation_profile(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "preparation-palette") as directory:
        tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
        tokens["themes"]["dark"] = json.loads(
            (PEERS / "fixtures/raw-dark-palette.json").read_text()
        )
        source = directory / "profile.c"
        source.write_text(fplinux_ui_profile.render(tokens))
        yield source


@pytest.fixture(scope="module")
def preparation(
    tmp_path_factory: pytest.TempPathFactory, preparation_profile: Path
) -> Iterator[PreparationBinaries]:
    with c_build_directory(tmp_path_factory, "preparation") as directory:
        command = directory / "preparation"
        status = directory / "status"
        small_font = directory / "small.psf"
        large_font = directory / "large.psf"
        write_solid_ascii_font(small_font, width=6, height=12)
        write_solid_ascii_font(large_font, width=8, height=16)
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
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                str(preparation_profile),
                str(PEERS / "preparation-session.c"),
                "-Wl,--wrap=fopen,--wrap=fplinux_font_glyph",
                *json_flags,
            ],
            name="compile preparation command with simulated DRM",
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
        yield PreparationBinaries(command, status, small_font, large_font)


@pytest.mark.parametrize("preparation_profile", ["variant"], indirect=True)
@pytest.mark.parametrize(("width", "height"), [(128, 160), (176, 220), (240, 320), (320, 240)])
def test_dark_palette_tokens_reach_reading_pixels(
    preparation: PreparationBinaries, tmp_path: Path, width: int, height: int
) -> None:
    status = tmp_path / "status.json"
    update_status(preparation, status, "reading", "--completed", "33554432", "--total", "67108864")
    environment = display_environment(preparation, tmp_path, width, height)

    def inspect_palette(process: subprocess.Popen[str], deadline: float) -> None:
        assert next_event(process, deadline) == "opened"
        assert next_frame(process, deadline) == (
            "FPLinuxFirststartPhonebackup50%32.0/64.0MiBKeepthephoneconnected."
        )
        pixels = struct.unpack(f"<{width * height}H", (tmp_path / "frame").read_bytes())
        font_width, font_height = (6, 12) if width < 200 else (8, 16)
        glyph_area = font_width * font_height
        half_bar_area = (width - 2 * font_width) // 2 * (font_height // 2)
        assert set(pixels) == {0x0000, 0x07E0, 0x001F, 0x8010, 0xF800}
        assert pixels.count(0x07E0) == 18 * glyph_area
        assert pixels.count(0x001F) == 44 * glyph_area
        assert pixels.count(0x8010) == 3 * glyph_area + half_bar_area
        assert pixels.count(0xF800) == half_bar_area
        process.send_signal(signal.SIGTERM)

    result = run_process(
        [str(preparation.command), str(status)],
        name=f"render generated dark palette on {width}x{height}",
        timeout=5,
        env=environment,
        while_running=inspect_palette,
    )
    assert result.returncode == 143, result.stderr
    assert result.stdout == "closed\n"
    assert result.stderr == ""


def update_status(
    preparation: PreparationBinaries, status: Path, stage: str, *counts: str
) -> None:
    result = run_process(
        [str(preparation.command), "--stage", stage, *counts, str(status)],
        name=f"write preparation stage {stage} without opening devices",
        timeout=5,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == ""


@pytest.mark.parametrize("stage", ["waiting", "extracting", "ready", "error", "cancelled"])
def test_stage_writer_needs_no_display(
    preparation: PreparationBinaries, tmp_path: Path, stage: str
) -> None:
    status = tmp_path / "status.json"
    update_status(preparation, status, stage)
    assert json.loads(status.read_bytes()) == {"stage": stage}
    assert sorted(path.name for path in tmp_path.iterdir()) == ["status.json"]


@pytest.mark.parametrize(
    ("completed", "total"),
    [(0, 67108864), (33554432, 67108864), (67108864, 67108864), (2**64 - 1, 2**64 - 1)],
)
def test_reading_writer_preserves_exact_byte_counts(
    preparation: PreparationBinaries, tmp_path: Path, completed: int, total: int
) -> None:
    status = tmp_path / "status.json"
    update_status(
        preparation,
        status,
        "reading",
        "--completed",
        str(completed),
        "--total",
        str(total),
    )
    assert json.loads(status.read_bytes()) == {
        "stage": "reading",
        "completed": completed,
        "total": total,
    }
    result = run_process(
        [str(preparation.status), str(status)],
        name="read the exact byte counts through production JSON code",
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
def test_invalid_updates_preserve_the_previous_status(
    preparation: PreparationBinaries, tmp_path: Path, arguments: list[str]
) -> None:
    status = tmp_path / "status.json"
    previous = b'{"stage":"waiting"}\n'
    status.write_bytes(previous)
    result = run_process(
        [str(preparation.command), *arguments, str(status)],
        name="reject invalid preparation values before file or device access",
        timeout=5,
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "--help' for more information." in result.stderr
    assert status.read_bytes() == previous


def test_help_precedes_invalid_values_and_resource_access(
    preparation: PreparationBinaries,
) -> None:
    result = run_process(
        [str(preparation.command), "--stage", "invalid", "--help"],
        name="show preparation help without status or display access",
        timeout=5,
    )
    assert result.returncode == 0
    assert "STATUS_FILE" in result.stdout
    assert "--completed=BYTES" in result.stdout
    assert result.stderr == ""


def test_failed_publish_removes_its_temporary_file(
    preparation: PreparationBinaries, tmp_path: Path
) -> None:
    status = tmp_path / "status.json"
    status.mkdir()
    result = run_process(
        [str(preparation.command), "--stage", "ready", str(status)],
        name="fail preparation publication at a directory destination",
        timeout=5,
    )
    assert result.returncode == 1
    assert "cannot write status" in result.stderr
    assert list(tmp_path.iterdir()) == [status]
    assert status.is_dir()


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
def test_malformed_status_preserves_the_last_valid_state(
    preparation: PreparationBinaries, tmp_path: Path, payload: bytes
) -> None:
    status = tmp_path / "status.json"
    status.write_bytes(payload)
    result = run_process(
        [str(preparation.status), str(status)],
        name="read malformed preparation status without replacing valid state",
        timeout=5,
        check=True,
    )
    assert result.stdout == "invalid 3 41 42\n"


def test_missing_status_means_waiting(preparation: PreparationBinaries, tmp_path: Path) -> None:
    result = run_process(
        [str(preparation.status), str(tmp_path / "missing")],
        name="read the initial absent preparation status",
        timeout=5,
        check=True,
    )
    assert result.stdout == "valid 0 0 0\n"


def next_line(descriptor: int, deadline: float) -> str:
    """Read complete events without buffering ahead of a later communicate()."""
    value = bytearray()
    while True:
        readable, _, _ = select.select([descriptor], [], [], max(0, deadline - time.monotonic()))
        assert readable, "preparation display did not report its next event"
        byte = os.read(descriptor, 1)
        assert byte, "preparation display exited before its next event"
        if byte == b"\n":
            return value.decode("ascii")
        value.extend(byte)


def next_event(process: subprocess.Popen[str], deadline: float) -> str:
    assert process.stdout is not None
    return next_line(process.stdout.fileno(), deadline)


def next_frame(process: subprocess.Popen[str], deadline: float) -> str:
    while True:
        event = next_event(process, deadline)
        if event.startswith("frame "):
            return event.split(" ", 2)[2]


def display_environment(
    preparation: PreparationBinaries, tmp_path: Path, width: int, height: int
) -> dict[str, str]:
    vt = tmp_path / "vt"
    os.mkfifo(vt)
    return {
        **os.environ,
        "FPLINUX_TEST_FONT": str(
            preparation.small_font if width < 200 else preparation.large_font
        ),
        "FPLINUX_TEST_GEOMETRY": f"{width}x{height}",
        "FPLINUX_TEST_VT": str(vt),
        "FPLINUX_TEST_FRAME": str(tmp_path / "frame"),
    }


@pytest.mark.parametrize(("width", "height"), [(128, 160), (240, 320), (320, 240)])
@pytest.mark.parametrize(
    ("completed", "percent"), [(0, "0%"), (33554432, "50%"), (67108864, "100%")]
)
def test_reading_renders_real_progress_and_complete_text(  # noqa: PLR0913 -- geometry and counters are independent inputs.
    preparation: PreparationBinaries,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    completed: int,
    percent: str,
) -> None:
    status = tmp_path / "status.json"
    update_status(
        preparation, status, "reading", "--completed", str(completed), "--total", "67108864"
    )
    environment = display_environment(preparation, tmp_path, width, height)
    label = f"{completed / 1048576:.1f}/64.0MiB"
    expected = f"FPLinuxFirststartPhonebackup{percent}{label}Keepthephoneconnected."

    def inspect_frame(process: subprocess.Popen[str], deadline: float) -> None:
        assert next_event(process, deadline) == "opened"
        assert next_frame(process, deadline) == expected
        pixels = struct.unpack(f"<{width * height}H", (tmp_path / "frame").read_bytes())
        font_width, font_height = (6, 12) if width < 200 else (8, 16)
        bar_width = width - 2 * font_width
        filled = int(percent[:-1]) * bar_width // 100
        bar_rows = []
        for row in range(height):
            inside = pixels[row * width + font_width : (row + 1) * width - font_width]
            if all(pixel in {0xAE7B, 0x3A29} for pixel in inside):
                bar_rows.append(inside)
        assert bar_rows
        assert all(
            row == (0xAE7B,) * filled + (0x3A29,) * (bar_width - filled) for row in bar_rows
        )
        assert sum(pixel != 0x1082 for pixel in pixels) == (
            len(expected) * font_width * font_height + bar_width * (font_height // 2)
        )
        process.send_signal(signal.SIGTERM)

    result = run_process(
        [str(preparation.command), str(status)],
        name=f"render first-start progress on {width}x{height}",
        timeout=5,
        env=environment,
        while_running=inspect_frame,
    )
    assert result.returncode == 143, result.stderr
    assert result.stdout == "closed\n"
    assert result.stderr == ""


@pytest.mark.parametrize(("width", "height"), [(128, 160), (240, 320), (320, 240)])
@pytest.mark.parametrize(
    ("stage", "content", "animated"),
    [
        ("waiting", "ConnectingKeepthephoneconnectedtothecomputer.", True),
        ("extracting", "PreparingfilesWorkingonthecomputer.Pleasewait.", True),
        ("ready", "ReadyPoweroffthephone,thenrunthesamelauncheragain.", False),
        ("error", "PreparationfailedCheckthecomputerlog,thentryagain.", False),
        ("cancelled", "CancelledCheckthecomputerlog,thentryagain.", False),
    ],
)
def test_stage_screens_fit_and_animate_only_busy_work(  # noqa: PLR0913 -- each screen has explicit geometry and expected content.
    preparation: PreparationBinaries,
    tmp_path: Path,
    *,
    width: int,
    height: int,
    stage: str,
    content: str,
    animated: bool,
) -> None:
    status = tmp_path / "status.json"
    update_status(preparation, status, stage)
    expected = f"FPLinuxFirststart{content}"
    environment = display_environment(preparation, tmp_path, width, height)

    def inspect_stage(process: subprocess.Popen[str], deadline: float) -> None:
        assert next_event(process, deadline) == "opened"
        assert next_frame(process, deadline) == expected
        frame = (tmp_path / "frame").read_bytes()
        pixels = struct.unpack(f"<{width * height}H", frame)
        font_width, font_height = (6, 12) if width < 200 else (8, 16)
        indicator_area = (width - 2 * font_width) * (font_height // 2) if animated else 0
        assert sum(pixel != 0x1082 for pixel in pixels) == (
            len(expected) * font_width * font_height + indicator_area
        )
        if animated:
            assert next_frame(process, deadline) == expected
            assert (tmp_path / "frame").read_bytes() != frame
        process.send_signal(signal.SIGTERM)

    result = run_process(
        [str(preparation.command), str(status)],
        name=f"render preparation stage {stage} on {width}x{height}",
        timeout=5,
        env=environment,
        while_running=inspect_stage,
    )
    assert result.returncode == 143, result.stderr
    assert result.stdout == "closed\n"
    assert result.stderr == ""


def test_display_tracks_replacement_partial_files_terminal_stages_and_vt(
    preparation: PreparationBinaries, tmp_path: Path
) -> None:
    status = tmp_path / "status.json"
    environment = display_environment(preparation, tmp_path, 128, 160)

    def inspect_updates(process: subprocess.Popen[str], deadline: float) -> None:
        assert next_event(process, deadline) == "opened"
        assert "Connecting" in next_frame(process, deadline)
        update_status(
            preparation, status, "reading", "--completed", "33554432", "--total", "67108864"
        )
        assert "Phonebackup50%32.0/64.0MiB" in next_frame(process, deadline)
        # A non-atomic incomplete write must not replace the valid reading view.
        reading_frame = (tmp_path / "frame").read_bytes()
        status.write_bytes(b'{"stage":')
        assert process.stderr is not None
        assert (
            next_line(process.stderr.fileno(), deadline)
            == "preparation: cannot read status: Invalid argument"
        )
        assert (tmp_path / "frame").read_bytes() == reading_frame
        update_status(preparation, status, "extracting")
        assert "PreparingfilesWorkingonthecomputer.Pleasewait." in next_frame(process, deadline)
        for stage, heading in [
            ("ready", "Ready"),
            ("error", "Preparationfailed"),
            ("cancelled", "Cancelled"),
        ]:
            update_status(preparation, status, stage)
            rendered = next_frame(process, deadline)
            assert heading in rendered
            if stage == "ready":
                assert "Poweroffthephone,thenrunthesamelauncheragain." in rendered
            else:
                assert "Checkthecomputerlog,thentryagain." in rendered
            assert "%" not in rendered
            assert "MiB" not in rendered
        terminal_frame = (tmp_path / "frame").read_bytes()
        with (tmp_path / "vt").open("wb", buffering=0) as vt:
            vt.write(b"0")
            assert next_event(process, deadline) == "inactive"
            vt.write(b"1")
            assert next_event(process, deadline) == "active"
            assert "Cancelled" in next_frame(process, deadline)
        assert (tmp_path / "frame").read_bytes() == terminal_frame
        status.unlink()
        assert "Connecting" in next_frame(process, deadline)
        process.send_signal(signal.SIGINT)

    result = run_process(
        [str(preparation.command), str(status)],
        name="observe preparation file updates and restore the simulated VT",
        timeout=10,
        env=environment,
        while_running=inspect_updates,
    )
    assert result.returncode == 130, result.stderr
    assert result.stdout == "closed\n"
    assert all(
        line == "preparation: cannot read status: Invalid argument"
        for line in result.stderr.splitlines()
    )
