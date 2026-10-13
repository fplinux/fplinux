# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral host-tool tests for the FPLinux charge wrapper."""

from __future__ import annotations

import os
import re
import signal
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator
    from pathlib import Path

SOURCE = ROOT / "alpine/aports/fplinux-charge/fplinux-charge.c"


@dataclass(frozen=True)
class ChargeTools:
    """Linked wrapper variants for direct and discovered counter paths."""

    executable: Path
    discovery_executable: Path


class FPLinuxChargeHostToolTests:
    """Compile and execute the production wrapper against a private counter."""

    @pytest.fixture(scope="class")
    @classmethod
    def tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[ChargeTools]:
        """Compile one production binary with the test-owned counter path."""
        with c_build_directory(tmp_path_factory, "fplinux-charge") as directory:
            executable = directory / "fplinux-charge"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    '-DFPLINUX_CHARGE_COUNTER_PATH="charge_counter"',
                    str(SOURCE),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                ],
                name="compile fplinux-charge",
                check=True,
            )
            discovery_executable = directory / "fplinux-charge-discovery"
            compile_c(
                discovery_executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    '-DFPLINUX_CHARGE_COUNTER_GLOB="power_supply/*/charge_counter"',
                    str(SOURCE),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                ],
                name="compile fplinux-charge discovery",
                check=True,
            )
            yield ChargeTools(executable, discovery_executable)

    def add_battery(self, work: Path, charge_counter: int) -> Path:
        """Create the test-owned counter and return its path."""
        counter = work / "charge_counter"
        counter.write_text(f"{charge_counter}\n", encoding="ascii")
        return counter

    def run_charge(
        self,
        executable: Path,
        work: Path,
        command: list[str],
        *,
        timeout: float = 5,
    ) -> subprocess.CompletedProcess[str]:
        """Run the wrapper through the bounded test process boundary."""
        return run_process(
            [str(executable), "--", *command],
            name="run fplinux-charge",
            timeout=timeout,
            cwd=work,
        )

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

    def test_reports_charge_delta_after_successful_command(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """A successful command produces its real counter delta and exits zero."""
        counter = self.add_battery(tmp_path, 1000)
        sentinel = tmp_path / "happy-command-ran"

        result = self.run_charge(
            tools.executable,
            tmp_path,
            [
                "/bin/sh",
                "-c",
                'printf "1120\\n" > "$1"; printf ran > "$2"',
                "fplinux-charge-test",
                str(counter),
                str(sentinel),
            ],
        )

        assert (result.returncode) == (0), result.stderr
        assert (sentinel.read_text(encoding="ascii")) == ("ran")
        assert (
            re.search(
                r"^fplinux-charge: elapsed=\d+\.\d{3} s "
                r"charge_delta=\+120 uAh average_current=\+\d+ uA\n$",
                result.stderr,
            )
            is not None
        )

    @pytest.mark.parametrize(
        "arguments",
        [["-h"], ["--help"], ["--unknown", "--help"]],
        ids=["h", "help", "unknown-help"],
    )
    def test_help_exits_without_reading_a_counter(
        self, tools: ChargeTools, tmp_path: Path, arguments: list[str]
    ) -> None:
        """Recognized help takes priority even when a command or option is invalid."""
        result = run_process(
            [str(tools.executable), *arguments],
            name="read fplinux-charge help without a counter",
            timeout=5,
            cwd=tmp_path,
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert ("Usage:") in (result.stdout)
        assert ("--help") in (result.stdout)
        assert ("--counter") in (result.stdout)
        assert ("command") in (result.stdout)

    def test_help_does_not_execute_the_child(self, tools: ChargeTools, tmp_path: Path) -> None:
        """Help exits before a valid counter and child command can have effects."""
        self.add_battery(tmp_path, 1000)
        sentinel = tmp_path / "help-command-ran"

        result = run_process(
            [
                str(tools.executable),
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
            cwd=tmp_path,
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert not (sentinel.exists())

    @pytest.mark.parametrize(
        "arguments",
        [
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
        ],
        ids=[
            "empty",
            "separator",
            "separator-empty",
            "bin-true",
            "unknown-separator-bin-true",
            "counter",
            "counter-separator-bin-true",
            "counter-empty-separator-bin-true",
            "counter-first-counter-second-separator-bin-true",
            "counter-separator-bin-true-2",
            "unexpected-separator-bin-true",
            "counter-counter-unexpected-separator-bin-true",
        ],
    )
    def test_invalid_arguments_fail_before_measurement(
        self, tools: ChargeTools, tmp_path: Path, arguments: list[str]
    ) -> None:
        """Bad options or a missing command separator are syntax errors."""
        result = run_process(
            [str(tools.executable), *arguments],
            name="reject invalid fplinux-charge arguments",
            timeout=5,
            cwd=tmp_path,
        )

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)
        assert ("cannot read") not in (result.stderr)

    def test_child_arguments_are_preserved_after_the_separator(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """Child options, empty arguments and separators reach the child unchanged."""
        self.add_battery(tmp_path, 1000)

        result = self.run_charge(
            tools.executable,
            tmp_path,
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
            ],
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == (
            "<--help>\n<--counter>\n<two words>\n<>\n<-->\n<--counter=child>\n"
        )
        assert ("charge_delta=+0 uAh") in (result.stderr)

    @pytest.mark.parametrize("name", ["--", "--help"], ids=["separator", "help"])
    def test_counter_value_can_look_like_an_option(
        self, tools: ChargeTools, tmp_path: Path, name: str
    ) -> None:
        """Counter values named -- or --help are paths, with a separate child delimiter."""
        (tmp_path / name).write_text("1000\n", encoding="ascii")
        result = run_process(
            [str(tools.executable), "--counter", name, "--", "/bin/true"],
            name="run fplinux-charge with an option-shaped counter path",
            timeout=5,
            cwd=tmp_path,
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == ("")
        assert ("charge_delta=+0 uAh") in (result.stderr)

    def test_preserves_child_exit_status(self, tools: ChargeTools, tmp_path: Path) -> None:
        """A normally exiting child keeps its nonzero status after measurement."""
        self.add_battery(tmp_path, 1000)

        result = self.run_charge(tools.executable, tmp_path, ["/bin/sh", "-c", "exit 37"])

        assert (result.returncode) == (37), result.stderr
        assert ("charge_delta=+0 uAh average_current=+0 uA") in (result.stderr)

    def test_sigterm_is_forwarded_and_terminates_wrapper(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """SIGTERM reaches a ready child and the wrapper terminates by SIGTERM."""
        self.add_battery(tmp_path, 1000)
        ready = tmp_path / "sigterm-child-ready"

        def terminate_when_ready(
            process: subprocess.Popen[str],
            deadline: float,
        ) -> None:
            while time.monotonic() < deadline:
                if ready.exists():
                    os.kill(process.pid, signal.SIGTERM)
                    return
                if process.poll() is not None:
                    pytest.fail("fplinux-charge exited before its child became ready")
                time.sleep(0.01)
            pytest.fail("fplinux-charge child did not publish its readiness marker")

        result = run_process(
            [
                str(tools.executable),
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
            cwd=tmp_path,
        )

        assert (result.returncode) == (-signal.SIGTERM), result.stderr
        assert ("charge_delta=+0 uAh average_current=+0 uA") in (result.stderr)

    def test_missing_counter_prevents_command_execution(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """The wrapper fails before executing the command without a battery counter."""
        sentinel = tmp_path / "missing-counter-command-ran"

        result = self.run_charge(
            tools.executable,
            tmp_path,
            [
                "/bin/sh",
                "-c",
                'printf ran > "$1"',
                "fplinux-charge-test",
                str(sentinel),
            ],
        )

        assert (result.returncode) == (125), result.stderr
        assert not (sentinel.exists())
        assert ("fplinux-charge: cannot read") in (result.stderr)

    def test_missing_command_returns_127(self, tools: ChargeTools, tmp_path: Path) -> None:
        """An execvp ENOENT is reported as the conventional status 127."""
        self.add_battery(tmp_path, 1000)
        missing_command = "fplinux-charge-command-that-does-not-exist"

        result = self.run_charge(tools.executable, tmp_path, [missing_command])

        assert (result.returncode) == (127), result.stderr
        assert (f"cannot execute {missing_command}:") in (result.stderr)
        assert ("charge_delta=+0 uAh average_current=+0 uA") in (result.stderr)

    def test_default_discovery_selects_only_the_battery_counter(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """The default class scan ignores non-Battery charge counters."""
        power_supply = tmp_path / "power_supply"
        battery_counter = self.add_power_supply(power_supply, "battery", "Battery", 1000)
        self.add_power_supply(power_supply, "charger", "USB", 2000)
        executable = tools.discovery_executable

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
            cwd=tmp_path,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("charge_delta=+120 uAh") in (result.stderr)

    def test_default_discovery_rejects_multiple_battery_counters(
        self, tools: ChargeTools, tmp_path: Path
    ) -> None:
        """Ambiguous Battery telemetry prevents the command from starting."""
        power_supply = tmp_path / "power_supply"
        self.add_power_supply(power_supply, "battery0", "Battery", 1000)
        self.add_power_supply(power_supply, "battery1", "Battery", 2000)
        executable = tools.discovery_executable
        sentinel = tmp_path / "ambiguous-command-ran"

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
            cwd=tmp_path,
        )

        assert (result.returncode) == (125), result.stderr
        assert not (sentinel.exists())
        assert ("multiple Battery charge counters") in (result.stderr)

    @pytest.mark.parametrize("attached", [False, True], ids=["False", "True"])
    def test_explicit_counter_overrides_default_discovery(
        self, tools: ChargeTools, tmp_path: Path, *, attached: bool
    ) -> None:
        """A caller can select a known counter without class discovery."""
        counter = tmp_path / "explicit_counter"
        counter.write_text("1000\n", encoding="ascii")
        arguments = [f"--counter={counter}"] if attached else ["--counter", str(counter)]
        result = run_process(
            [str(tools.executable), *arguments, "--", "/bin/true"],
            name="run fplinux-charge explicit counter",
            timeout=5,
            cwd=tmp_path,
        )
        assert result.returncode == 0, result.stderr
        assert "charge_delta=+0 uAh" in result.stderr
