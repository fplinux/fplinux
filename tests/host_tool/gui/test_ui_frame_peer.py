# SPDX-License-Identifier: GPL-2.0-only
"""The screenshot compositor preserves acknowledged xdg-shell geometry."""

from __future__ import annotations

import signal
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def frame_peer(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "ui-frame-peer") as directory:
        protocol = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
        for kind, filename in (
            ("client-header", "xdg-shell-client-protocol.h"),
            ("server-header", "xdg-shell-server-protocol.h"),
            ("private-code", "xdg-shell-protocol.c"),
        ):
            run_process(
                ["wayland-scanner", kind, protocol, str(directory / filename)],
                name=f"generate frame peer xdg-shell {kind}",
                timeout=10,
                check=True,
            )
        flags = run_process(
            [
                "pkg-config",
                "--cflags",
                "--libs",
                "wayland-client",
                "wayland-server",
                "xkbcommon",
                "json-c",
            ],
            name="resolve frame peer protocol dependencies",
            timeout=10,
            check=True,
        ).stdout.split()
        executable = directory / "frame-peer"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'site/scripts/screens'}",
                f"-I{directory}",
                str(ROOT / "tests/host_tool/gui/ui-frame-peer-configure.c"),
                str(ROOT / "site/scripts/screens/ui-frame-peer.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                str(directory / "xdg-shell-protocol.c"),
                *flags,
            ],
            name="link screenshot compositor with a controlled Wayland client",
        )
        yield executable


def test_frame_peer_keeps_old_frames_until_resize_ack_and_new_commit(frame_peer: Path) -> None:
    result = run_process(
        [str(frame_peer), "resize"],
        name="commit old geometry before a pending resize acknowledgement",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "resize: old frame, resize ACK and new frame checked\n"


@pytest.mark.parametrize(
    "scenario", ["initial-ack", "serial", "duplicate-ack", "dimensions", "pre-ack-dimensions"]
)
def test_frame_peer_rejects_unacknowledged_or_mismatched_geometry(
    frame_peer: Path, scenario: str
) -> None:
    result = run_process(
        [str(frame_peer), scenario],
        name=f"reject invalid frame peer protocol: {scenario}",
        timeout=10,
    )
    assert result.stdout == f"invalid: {scenario}\n"
    assert result.returncode == -signal.SIGABRT, result.stdout + result.stderr
