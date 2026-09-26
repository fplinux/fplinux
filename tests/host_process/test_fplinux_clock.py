# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for the phone clock tool shipped in the SSH package."""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
CLOCK_TOOL = ROOT / "alpine/aports/fplinux-ssh/fplinux-clock"
CLOCK_FIXTURES = ROOT / "tests/fixtures/fplinux_clock"


@dataclass(frozen=True)
class ClockRun:
    """Observable result of one tool run and the clock commands it issued."""

    returncode: int
    stdout: str
    stderr: str
    commands: list[str]


class FPLinuxClockTests(unittest.TestCase):
    """Run the shipped script on the host with fake date and hwclock commands.

    The fakes replace the kernel system clock and the RTC device with test-owned
    files. These tests do not exercise BusyBox option parsing or a phone RTC.
    """

    def setUp(self) -> None:
        """Start from a freshly booted system clock and an RTC without readable time."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
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
        self.events = work / "events"

    def run_clock(
        self,
        *arguments: str,
        clock_set: str = "stored",
        rtc_write: str = "stored",
    ) -> ClockRun:
        """Run the tool once with the selected fake clock behavior."""
        environment = os.environ | {
            "PATH": f"{self.commands}:{os.environ['PATH']}",
            "FPLINUX_TEST_EVENTS": str(self.events),
            "FPLINUX_TEST_SYSTEM_CLOCK": str(self.system_clock),
            "FPLINUX_TEST_RTC": str(self.rtc),
            "FPLINUX_TEST_CLOCK_SET": clock_set,
            "FPLINUX_TEST_RTC_WRITE": rtc_write,
        }
        result = run_process(
            [str(CLOCK_TOOL), *arguments],
            name="fplinux-clock",
            timeout=5,
            env=environment,
        )
        commands = (
            self.events.read_text(encoding="utf-8").splitlines() if self.events.exists() else []
        )
        return ClockRun(result.returncode, result.stdout, result.stderr, commands)

    def test_readable_rtc_keeps_its_time(self) -> None:
        """The system clock is set, and an RTC that can be read is not written."""
        self.rtc.write_text("time kept by another firmware\n", encoding="ascii")

        run = self.run_clock("1788739200")

        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "system=1788739200 rtc=kept\n")
        self.assertEqual(self.system_clock.read_text(encoding="ascii"), "1788739200\n")
        self.assertEqual(
            run.commands,
            [
                "date -u -s @1788739200",
                "date -u +%s",
                "hwclock -u -r -f /dev/rtc0",
            ],
        )
        self.assertEqual(self.rtc.read_text(encoding="ascii"), "time kept by another firmware\n")

    def test_unreadable_rtc_is_written_and_read_back(self) -> None:
        """An RTC without readable time receives the new UTC system time."""
        run = self.run_clock("1788739200")

        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "system=1788739200 rtc=written\n")
        self.assertEqual(self.rtc.read_text(encoding="ascii"), "1788739200\n")
        self.assertEqual(
            run.commands,
            [
                "date -u -s @1788739200",
                "date -u +%s",
                "hwclock -u -r -f /dev/rtc0",
                "hwclock -u -w -f /dev/rtc0",
                "hwclock -u -r -f /dev/rtc0",
            ],
        )

    def test_rtc_that_stays_unreadable_fails_after_setting_the_system_clock(self) -> None:
        """A rejected write or a write that is still unreadable is reported as failed."""
        for rtc_write in ("rejected", "unreadable"):
            with self.subTest(rtc_write=rtc_write):
                self.events.unlink(missing_ok=True)

                run = self.run_clock("1788739200", rtc_write=rtc_write)

                self.assertEqual(run.returncode, 1)
                self.assertEqual(run.stdout, "system=1788739200 rtc=failed\n")
                self.assertTrue(
                    run.stderr.endswith(
                        "fplinux-clock: cannot store a readable time in /dev/rtc0\n"
                    ),
                    run.stderr,
                )
                self.assertEqual(self.system_clock.read_text(encoding="ascii"), "1788739200\n")
                self.assertFalse(self.rtc.exists())

    def test_rejected_system_time_stops_before_the_rtc(self) -> None:
        """A date command that exits 0 without changing the clock is detected by readback."""
        run = self.run_clock("1788739200", clock_set="ignored")

        self.assertEqual(run.returncode, 1)
        self.assertEqual(run.stdout, "")
        self.assertTrue(
            run.stderr.endswith("fplinux-clock: system clock reads 0 after setting 1788739200\n"),
            run.stderr,
        )
        self.assertEqual(run.commands, ["date -u -s @1788739200", "date -u +%s"])

    def test_invalid_argument_changes_no_clock(self) -> None:
        """Only one decimal UTC seconds argument is accepted."""
        cases: tuple[tuple[str, ...], ...] = (
            (),
            ("",),
            ("-1788739200",),
            ("@1788739200",),
            ("1788739200.5",),
            ("1788739200", "1788739201"),
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                run = self.run_clock(*arguments)

                self.assertEqual(run.returncode, 2)
                self.assertEqual(run.stdout, "")
                self.assertEqual(run.stderr, "usage: fplinux-clock UTC_SECONDS\n")
                self.assertEqual(run.commands, [])


if __name__ == "__main__":
    unittest.main()
