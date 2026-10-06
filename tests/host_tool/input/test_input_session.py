# SPDX-License-Identifier: GPL-2.0-only
"""Input-session components with controlled udev, ioctl and libevdev boundaries.

Real epoll, pipes and eventfd exercise readiness. Library synchronization is
stubbed; these checks do not create a kernel input device or prove kernel loss
recovery, hotplug or grabs.
"""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class InputSessionTests:
    """Observe the production session's caller-visible event sequence."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "input-session"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/input/input-session-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    str(ROOT / "lib/fplinux/fplinux-input-session.c"),
                    str(ROOT / "tests/host_tool/input/fplinux-input-session.c"),
                    "-Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=nanosleep",
                    "-o",
                    str(cls.executable),
                ],
                name="compile input session with fake device boundaries",
                timeout=30,
                check=True,
            )
            yield

    @pytest.mark.parametrize(
        "scenario",
        [
            "lifecycle",
            "classification",
            "rejected",
            "modifiers",
            "frames",
            "pointer",
            "sync",
            "retry",
        ],
        ids=[
            "lifecycle",
            "classification",
            "rejected",
            "modifiers",
            "frames",
            "pointer",
            "sync",
            "retry",
        ],
    )
    def test_event_translation_and_device_lifetimes(self, scenario: str) -> None:
        """Literal traces preserve identity, frames, repeat and synchronized state."""
        run_process(
            [str(self.executable), scenario],
            name=f"run input-session {scenario} component scenario",
            timeout=5,
            check=True,
        )
