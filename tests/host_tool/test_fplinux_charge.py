# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral host-tool tests for the FPLinux charge wrapper."""

from __future__ import annotations

import os
import signal
import tempfile
import time
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "alpine/aports/fplinux-charge/fplinux-charge.c"


class FPLinuxChargeHostToolTests(unittest.TestCase):
    """Compile and execute the production wrapper against a private counter."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    work: ClassVar[Path]
    counter: ClassVar[Path]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile one production binary with the test-owned counter path."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.counter = cls.work / "charge_counter"
        cls.executable = cls.work / "fplinux-charge"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "alpine/shared"),
                f'-DFPLINUX_CHARGE_COUNTER_PATH="{cls.counter}"',
                str(SOURCE),
                str(ROOT / "alpine/shared" / "fplinux-cli.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile fplinux-charge",
            timeout=30,
            check=True,
        )

    def setUp(self) -> None:
        """Remove the fake counter before each behavior."""
        self.counter.unlink(missing_ok=True)

    def add_battery(self, charge_counter: int) -> Path:
        """Create the test-owned counter and return its path."""
        self.counter.write_text(f"{charge_counter}\n", encoding="ascii")
        return self.counter

    def run_charge(
        self,
        command: list[str],
        *,
        timeout: float = 5,
    ) -> subprocess.CompletedProcess[str]:
        """Run the wrapper through the bounded test process boundary."""
        return run_process(
            [str(self.executable), "--", *command],
            name="run fplinux-charge",
            timeout=timeout,
        )

    def compile_discovery_wrapper(self, power_supply: Path) -> Path:
        """Compile the production wrapper with a test-owned sysfs glob."""
        executable = self.work / "fplinux-charge-discovery"
        counter_glob = power_supply / "*" / "charge_counter"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "alpine/shared"),
                f'-DFPLINUX_CHARGE_COUNTER_GLOB="{counter_glob}"',
                str(SOURCE),
                str(ROOT / "alpine/shared" / "fplinux-cli.c"),
                "-o",
                str(executable),
            ],
            name="compile fplinux-charge discovery",
            timeout=30,
            check=True,
        )
        return executable

    def add_power_supply(
        self,
        root: Path,
        name: str,
        supply_type: str,
        charge_counter: int,
    ) -> Path:
        """Create one fake power-supply class device with a counter."""
        device = root / name
        device.mkdir(parents=True)
        (device / "type").write_text(f"{supply_type}\n", encoding="ascii")
        (device / "charge_counter").write_text(f"{charge_counter}\n", encoding="ascii")
        return device / "charge_counter"

    def test_reports_charge_delta_after_successful_command(self) -> None:
        """A successful command produces its real counter delta and exits zero."""
        counter = self.add_battery(1000)
        sentinel = self.work / "happy-command-ran"

        result = self.run_charge(
            [
                "/bin/sh",
                "-c",
                'printf "1120\\n" > "$1"; printf ran > "$2"',
                "fplinux-charge-test",
                str(counter),
                str(sentinel),
            ]
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sentinel.read_text(encoding="ascii"), "ran")
        self.assertRegex(
            result.stderr,
            r"^fplinux-charge: elapsed=\d+\.\d{3} s "
            r"charge_delta=\+120 uAh average_current=\+\d+ uA\n$",
        )

    def test_help_exits_without_reading_a_counter(self) -> None:
        """Recognized help takes priority even when a command or option is invalid."""
        for arguments in (["-h"], ["--help"], ["--unknown", "--help"]):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="read fplinux-charge help without a counter",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertIn("Usage:", result.stdout)
                self.assertIn("--help", result.stdout)
                self.assertIn("--counter", result.stdout)
                self.assertIn("command", result.stdout)

    def test_help_does_not_execute_the_child(self) -> None:
        """Help exits before a valid counter and child command can have effects."""
        self.add_battery(1000)
        sentinel = self.work / "help-command-ran"

        result = run_process(
            [
                str(self.executable),
                "--help",
                "--",
                "/bin/sh",
                "-c",
                'printf ran > "$1"',
                "fplinux-charge-test",
                str(sentinel),
            ],
            name="read fplinux-charge help with a child command",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertFalse(sentinel.exists())

    def test_invalid_arguments_fail_before_measurement(self) -> None:
        """Bad options or a missing command separator are syntax errors."""
        cases: tuple[list[str], ...] = (
            [],
            ["--"],
            ["--", ""],
            ["/bin/true"],
            ["--unknown", "--", "/bin/true"],
            ["--counter"],
            ["--counter=", "--", "/bin/true"],
            ["--counter", "", "--", "/bin/true"],
            ["--counter", "first", "--counter=second", "--", "/bin/true"],
            ["--counter", "--", "/bin/true"],
            ["unexpected", "--", "/bin/true"],
            ["--counter", "counter", "unexpected", "--", "/bin/true"],
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments],
                    name="reject invalid fplinux-charge arguments",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("--help", result.stderr)
                self.assertNotIn("cannot read", result.stderr)

    def test_child_arguments_are_preserved_after_the_separator(self) -> None:
        """Child options, empty arguments and separators reach the child unchanged."""
        self.add_battery(1000)

        result = self.run_charge(
            [
                "/bin/sh",
                "-c",
                'printf "<%s>\\n" "$@"',
                "fplinux-charge-test",
                "--help",
                "--counter",
                "two words",
                "",
                "--",
                "--counter=child",
            ]
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "<--help>\n<--counter>\n<two words>\n<>\n<-->\n<--counter=child>\n",
        )
        self.assertIn("charge_delta=+0 uAh", result.stderr)

    def test_counter_value_can_look_like_an_option(self) -> None:
        """Counter values named -- or --help are paths, with a separate child delimiter."""
        for name in ("--", "--help"):
            with self.subTest(name=name):
                (self.work / name).write_text("1000\n", encoding="ascii")
                result = run_process(
                    [str(self.executable), "--counter", name, "--", "/bin/true"],
                    name="run fplinux-charge with an option-shaped counter path",
                    timeout=5,
                    cwd=self.work,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("charge_delta=+0 uAh", result.stderr)

    def test_preserves_child_exit_status(self) -> None:
        """A normally exiting child keeps its nonzero status after measurement."""
        self.add_battery(1000)

        result = self.run_charge(["/bin/sh", "-c", "exit 37"])

        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertIn("charge_delta=+0 uAh average_current=+0 uA", result.stderr)

    def test_sigterm_is_forwarded_and_terminates_wrapper(self) -> None:
        """SIGTERM reaches a ready child and the wrapper terminates by SIGTERM."""
        self.add_battery(1000)
        ready = self.work / "sigterm-child-ready"

        def terminate_when_ready(
            process: subprocess.Popen[str],
            deadline: float,
        ) -> None:
            while time.monotonic() < deadline:
                if ready.exists():
                    os.kill(process.pid, signal.SIGTERM)
                    return
                if process.poll() is not None:
                    self.fail("fplinux-charge exited before its child became ready")
                time.sleep(0.01)
            self.fail("fplinux-charge child did not publish its readiness marker")

        result = run_process(
            [
                str(self.executable),
                "--",
                "/bin/sh",
                "-c",
                'printf ready > "$1"; exec sleep 30',
                "fplinux-charge-test",
                str(ready),
            ],
            name="run fplinux-charge SIGTERM forwarding",
            timeout=5,
            while_running=terminate_when_ready,
        )

        self.assertEqual(result.returncode, -signal.SIGTERM, result.stderr)
        self.assertIn("charge_delta=+0 uAh average_current=+0 uA", result.stderr)

    def test_missing_counter_prevents_command_execution(self) -> None:
        """The wrapper fails before executing the command without a battery counter."""
        sentinel = self.work / "missing-counter-command-ran"

        result = self.run_charge(
            [
                "/bin/sh",
                "-c",
                'printf ran > "$1"',
                "fplinux-charge-test",
                str(sentinel),
            ]
        )

        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse(sentinel.exists())
        self.assertIn("fplinux-charge: cannot read", result.stderr)

    def test_missing_command_returns_127(self) -> None:
        """An execvp ENOENT is reported as the conventional status 127."""
        self.add_battery(1000)
        missing_command = "fplinux-charge-command-that-does-not-exist"

        result = self.run_charge([missing_command])

        self.assertEqual(result.returncode, 127, result.stderr)
        self.assertIn(f"cannot execute {missing_command}:", result.stderr)
        self.assertIn("charge_delta=+0 uAh average_current=+0 uA", result.stderr)

    def test_default_discovery_selects_only_the_battery_counter(self) -> None:
        """The default class scan ignores non-Battery charge counters."""
        power_supply = self.work / "power_supply-selected"
        battery_counter = self.add_power_supply(power_supply, "battery", "Battery", 1000)
        self.add_power_supply(power_supply, "charger", "USB", 2000)
        executable = self.compile_discovery_wrapper(power_supply)

        result = run_process(
            [
                str(executable),
                "--",
                "/bin/sh",
                "-c",
                'printf "1120\\n" > "$1"',
                "fplinux-charge-test",
                str(battery_counter),
            ],
            name="run fplinux-charge Battery discovery",
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("charge_delta=+120 uAh", result.stderr)

    def test_default_discovery_rejects_multiple_battery_counters(self) -> None:
        """Ambiguous Battery telemetry prevents the command from starting."""
        power_supply = self.work / "power_supply-ambiguous"
        self.add_power_supply(power_supply, "battery0", "Battery", 1000)
        self.add_power_supply(power_supply, "battery1", "Battery", 2000)
        executable = self.compile_discovery_wrapper(power_supply)
        sentinel = self.work / "ambiguous-command-ran"

        result = run_process(
            [
                str(executable),
                "--",
                "/bin/sh",
                "-c",
                'printf ran > "$1"',
                "fplinux-charge-test",
                str(sentinel),
            ],
            name="reject ambiguous fplinux-charge Battery counters",
            timeout=5,
        )

        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse(sentinel.exists())
        self.assertIn("multiple Battery charge counters", result.stderr)

    def test_explicit_counter_overrides_default_discovery(self) -> None:
        """A caller can select a known counter without class discovery."""
        counter = self.work / "explicit_counter"
        counter.write_text("1000\n", encoding="ascii")
        for arguments in (["--counter", str(counter)], [f"--counter={counter}"]):
            with self.subTest(arguments=arguments):
                result = run_process(
                    [str(self.executable), *arguments, "--", "/bin/true"],
                    name="run fplinux-charge explicit counter",
                    timeout=5,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("charge_delta=+0 uAh", result.stderr)


if __name__ == "__main__":
    unittest.main()
