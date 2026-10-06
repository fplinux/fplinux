# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for the phone clock tool shipped in the SSH package."""

from __future__ import annotations

import os
import shutil
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

CLOCK_TOOL = ROOT / "alpine/aports/fplinux-ssh/fplinux-clock"
CLOCK_FIXTURES = ROOT / "tests/fixtures/fplinux_clock"


class FPLinuxClockTests:
    """Run the shipped script on the host with fake date and hwclock commands.

    The fakes replace the kernel system clock and the RTC device with test-owned
    files. These tests do not exercise BusyBox option parsing or a phone RTC.
    """

    @pytest.fixture(autouse=True)
    def _prepare_case(self) -> Iterator[None]:
        """Start from a freshly booted system clock and an RTC without readable time."""
        with ExitStack() as cleanup:
            temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(temporary)
            work = Path(temporary.name)
            self.commands = work / "bin"
            self.commands.mkdir()
            for name in ("date", "hwclock"):
                fake = self.commands / name
                shutil.copyfile(CLOCK_FIXTURES / name, fake)
                fake.chmod(0o755)
            self.system_clock = work / "system-clock"
            self.system_clock.write_text("0\n", encoding="ascii")
            self.rtc = work / "rtc"
            yield

    def run_clock(
        self,
        *arguments: str,
        clock_set: str = "stored",
        rtc_write: str = "stored",
    ) -> subprocess.CompletedProcess[str]:
        """Run the tool once with the selected fake clock behavior."""
        environment = os.environ | {
            "PATH": f"{self.commands}:{os.environ['PATH']}",
            "FPLINUX_TEST_SYSTEM_CLOCK": str(self.system_clock),
            "FPLINUX_TEST_RTC": str(self.rtc),
            "FPLINUX_TEST_CLOCK_SET": clock_set,
            "FPLINUX_TEST_RTC_WRITE": rtc_write,
        }
        return run_process(
            [str(CLOCK_TOOL), *arguments],
            name="fplinux-clock",
            timeout=5,
            env=environment,
        )

    def test_readable_rtc_keeps_its_time(self) -> None:
        """The system clock is set, and an RTC that can be read is not written."""
        self.rtc.write_text("time kept by another firmware\n", encoding="ascii")

        run = self.run_clock("1788739200")

        assert (run.returncode) == (0), run.stderr
        assert (run.stdout) == ("system=1788739200 rtc=kept\n")
        assert (self.system_clock.read_text(encoding="ascii")) == ("1788739200\n")
        assert (self.rtc.read_text(encoding="ascii")) == ("time kept by another firmware\n")

    def test_unreadable_rtc_receives_the_system_time(self) -> None:
        """An RTC without readable time receives the new UTC system time."""
        run = self.run_clock("1788739200")

        assert (run.returncode) == (0), run.stderr
        assert (run.stdout) == ("system=1788739200 rtc=written\n")
        assert (self.rtc.read_text(encoding="ascii")) == ("1788739200\n")

    @pytest.mark.parametrize(
        "rtc_write", ["rejected", "unreadable"], ids=["rejected-write", "unreadable-write"]
    )
    def test_rtc_that_stays_unreadable_fails_after_setting_the_system_clock(
        self, rtc_write: str
    ) -> None:
        """A rejected write or a write that is still unreadable is reported as failed."""
        run = self.run_clock("1788739200", rtc_write=rtc_write)

        assert (run.returncode) == (1)
        assert (run.stdout) == ("system=1788739200 rtc=failed\n")
        assert run.stderr.endswith("fplinux-clock: cannot store a readable time in /dev/rtc0\n"), (
            run.stderr
        )
        assert (self.system_clock.read_text(encoding="ascii")) == ("1788739200\n")
        assert not (self.rtc.exists())

    def test_rejected_system_time_stops_before_the_rtc(self) -> None:
        """A date command that exits 0 without changing the clock is detected by readback."""
        run = self.run_clock("1788739200", clock_set="ignored")

        assert (run.returncode) == (1)
        assert (run.stdout) == ("")
        assert run.stderr.endswith(
            "fplinux-clock: system clock reads 0 after setting 1788739200\n"
        ), run.stderr
        assert not (self.rtc.exists())

    @pytest.mark.parametrize(
        "arguments",
        [
            (),
            ("",),
            ("-1788739200",),
            ("@1788739200",),
            ("1788739200.5",),
            ("1788739200", "1788739201"),
        ],
        ids=["missing", "empty", "negative", "at-prefix", "fractional", "extra"],
    )
    def test_invalid_argument_changes_no_clock(self, arguments: tuple[str, ...]) -> None:
        """Only one decimal UTC seconds argument is accepted."""
        run = self.run_clock(*arguments)

        assert (run.returncode) == (2)
        assert (run.stdout) == ("")
        assert (run.stderr) == ("usage: fplinux-clock UTC_SECONDS\n")
        assert (self.system_clock.read_text(encoding="ascii")) == ("0\n")
        assert not (self.rtc.exists())
