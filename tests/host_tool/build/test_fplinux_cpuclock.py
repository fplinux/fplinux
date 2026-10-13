# SPDX-License-Identifier: GPL-2.0-only
"""Host CLI checks; the host loop cannot verify a phone frequency measurement."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

SOURCE = ROOT / "alpine/aports/fplinux-cpuclock/fplinux-cpuclock.c"
SHARED_INCLUDE = ROOT / "include/fplinux"


class FplinuxCpuclockCliTests:
    """Exercise argument validation and small counts using the host fallback loop."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Compile the production entry point with its host implementation."""
        with c_build_directory(tmp_path_factory, "fplinux-cpuclock") as temporary:
            executable = temporary / "fplinux-cpuclock"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED_INCLUDE}",
                    str(SOURCE),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                ],
                name="compile CPU clock host CLI",
                check=True,
            )
            yield executable

    @pytest.mark.parametrize(
        "arguments",
        [("-h",), ("--help",), ("0", "--help"), ("--unknown", "--help")],
        ids=["h", "help", "0-help", "unknown-help"],
    )
    def test_help_exits_before_measurement(
        self, executable: Path, arguments: tuple[str, ...]
    ) -> None:
        """Help is available with missing or malformed counts and extra options."""
        result = run_process(
            [str(executable), *arguments],
            name="run CPU clock help",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("Usage:") in (result.stdout)
        assert ("iterations") in (result.stdout)
        assert (result.stderr) == ("")
        assert ("round 1:") not in (result.stdout)

    @pytest.mark.parametrize(
        "arguments",
        [
            ("",),
            ("0",),
            ("-1",),
            ("1tail",),
            ("0x",),
            ("0x10",),
            ("4294967296",),
            ("18446744073709551616",),
            ("1", "0"),
            ("1", "4294967296"),
            ("1", "1tail"),
            ("1", "1", "extra"),
            ("--", "--help"),
        ],
        ids=[
            "empty",
            "0",
            "negative-one",
            "1tail",
            "0x",
            "0x10",
            "4294967296",
            "18446744073709551616",
            "1-0",
            "1-4294967296",
            "1-1tail",
            "1-1-extra",
            "separator-help",
        ],
    )
    def test_invalid_counts_and_extra_arguments_do_not_start_measurement(
        self, executable: Path, arguments: tuple[str, ...]
    ) -> None:
        """Reject partial numbers, zero and overflow before the ARM counter cast."""
        result = run_process(
            [str(executable), *arguments],
            name="run CPU clock argument error",
            timeout=5,
        )

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)

    def test_decimal_counts_select_the_requested_host_rounds(self, executable: Path) -> None:
        """Decimal iterations and rounds select the requested workload."""
        result = run_process(
            [str(executable), "16", "2"],
            name="run CPU clock host fallback",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert ("2 rounds of 16 x 256 dependent integer additions\n") in (result.stdout)
        assert ("round 1:") in (result.stdout)
        assert ("round 2:") in (result.stdout)
        assert ("round 3:") not in (result.stdout)
        assert ("best of 2 rounds:") in (result.stdout)

    def test_omitted_rounds_retain_five_measurements(self, executable: Path) -> None:
        """A positive signed iteration count leaves the round default unchanged."""
        result = run_process(
            [str(executable), "+16"],
            name="run CPU clock default host rounds",
            timeout=5,
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert ("5 rounds of 16 x 256 dependent integer additions\n") in (result.stdout)
        assert (result.stdout.count("fplinux-cpuclock: round ")) == (5)
        assert ("best of 5 rounds:") in (result.stdout)
