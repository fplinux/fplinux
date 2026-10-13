# SPDX-License-Identifier: GPL-2.0-only
"""Observe bridge diagnostics with controlled uinput, channel I/O and waits."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


class InputBridgeLoggingTests:
    """The executable owns retries and messages; no host keyboard is created."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-input-logging") as temporary:
            executable = temporary / "input-bridge"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "alpine/aports/fplinux-input/fplinux-input.c"),
                    str(ROOT / "tests/host_tool/input/fplinux-input-io.c"),
                    "-Wl,--wrap=open,--wrap=ioctl,--wrap=poll,--wrap=read,--wrap=sleep",
                ],
                name="compile input bridge with controlled devices",
                check=True,
            )
            yield executable

    def test_repeated_failure_is_quiet_until_data_resumes(self, executable: Path) -> None:
        """Report each distinct error once and announce both observed recoveries."""
        result = run_process([str(executable)], name="run bridge I/O scenario", timeout=5)
        assert (result.returncode) == (0), result.stderr
        errors = result.stderr.splitlines()
        assert (len(errors)) == (3), result.stderr
        assert all(line.startswith("fplinux-input: ") for line in errors)
        assert (sum("cannot open" in line for line in errors)) == (2)
        assert (sum("cannot poll" in line for line in errors)) == (1)
        assert (result.stdout.count("input channel readable")) == (2)
