# SPDX-License-Identifier: GPL-2.0-only
"""Observe bridge diagnostics with controlled uinput, channel I/O and waits."""

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


class InputBridgeLoggingTests:
    """The executable owns retries and messages; no host keyboard is created."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "input-bridge"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "alpine/aports/fplinux-input/fplinux-input.c"),
                    str(ROOT / "tests/host_tool/input/fplinux-input-io.c"),
                    "-Wl,--wrap=open,--wrap=ioctl,--wrap=poll,--wrap=read,--wrap=sleep",
                    "-o",
                    str(cls.executable),
                ],
                name="compile input bridge with controlled devices",
                timeout=30,
                check=True,
            )
            yield

    def test_repeated_failure_is_quiet_until_data_resumes(self) -> None:
        """Report each distinct error once and announce both observed recoveries."""
        result = run_process([str(self.executable)], name="run bridge I/O scenario", timeout=5)
        assert (result.returncode) == (0), result.stderr
        errors = result.stderr.splitlines()
        assert (len(errors)) == (3), result.stderr
        assert all(line.startswith("fplinux-input: ") for line in errors)
        assert (sum("cannot open" in line for line in errors)) == (2)
        assert (sum("cannot poll" in line for line in errors)) == (1)
        assert (result.stdout.count("input channel readable")) == (2)
