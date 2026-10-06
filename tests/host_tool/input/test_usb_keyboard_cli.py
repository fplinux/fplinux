# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral argument checks for the host USB keyboard bridge."""

from __future__ import annotations

import shlex
import subprocess
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SOURCE = ROOT / "common/host/fplinux-usb-keyboard.c"
SHARED = ROOT / "include/fplinux"


class FPLinuxUsbKeyboardCliTests:
    """Compile and exercise the real host tool without accessing USB devices."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Compile the production source with its declared host dependencies."""
        with ExitStack() as cleanup:
            try:
                libusb_flags = shlex.split(
                    run_process(
                        ["pkg-config", "--cflags", "--libs", "libusb-1.0"],
                        name="read libusb compiler and linker flags",
                        timeout=10,
                        check=True,
                    ).stdout
                )
            except (FileNotFoundError, subprocess.CalledProcessError) as error:
                message = "pkg-config libusb-1.0 development files are required"
                raise RuntimeError(message) from error

            cls.temporary = tempfile.TemporaryDirectory(prefix="fplinux-usb-keyboard-cli-")
            cleanup.enter_context(cls.temporary)
            cls.executable = Path(cls.temporary.name) / "fplinux-usb-keyboard"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(SHARED),
                    str(SOURCE),
                    *libusb_flags,
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-pthread",
                    "-o",
                    str(cls.executable),
                ],
                name="compile fplinux-usb-keyboard",
                timeout=30,
                check=True,
            )
            yield

    def run_keyboard(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run one bounded CLI-only or self-test invocation."""
        return run_process(
            [str(self.executable), *arguments],
            name="run fplinux-usb-keyboard argument check",
            timeout=5,
        )

    def assert_syntax_error(self, *arguments: str, message: str | None = None) -> None:
        """Require the shared stderr-only syntax-error contract."""
        result = self.run_keyboard(*arguments)
        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        if message is not None:
            assert (message) in (result.stderr)
        assert ("--help' for more information.") in (result.stderr)

    @pytest.mark.parametrize(
        "arguments",
        [
            ("-h",),
            ("--help",),
            ("--unknown", "--help"),
            ("--vid", "not-hex", "--help"),
            ("unexpected", "--help"),
        ],
        ids=["h", "help", "unknown-help", "vid-not-hex-help", "unexpected-help"],
    )
    def test_help_uses_the_argument_table_and_takes_priority(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Parsed help succeeds before syntax and semantic argument errors."""
        expected_help = self.run_keyboard("--help").stdout
        result = self.run_keyboard(*arguments)
        assert (result.returncode) == (0), result.stderr
        assert (result.stderr) == ("")
        assert (result.stdout) == (expected_help)
        assert expected_help.startswith(f"Usage: {self.executable}")
        assert ("Forward one Linux evdev keyboard") in (expected_help)
        for option in (
            "--vid",
            "--pid",
            "--interface",
            "--keyboard",
            "--bus",
            "--address",
            "--timeout-ms",
            "--wait",
            "--no-detach",
            "--list",
            "--self-test",
            "--help",
        ):
            assert (option) in (expected_help)

    def test_missing_value_does_not_turn_consumed_help_into_help(self) -> None:
        """An option value named --help remains that option's supplied value."""
        self.assert_syntax_error(
            "--vid",
            "--help",
            message="--vid must be a 16-bit hexadecimal value",
        )

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (("--unknown",), None),
            (("unexpected",), None),
            (("--timeout-ms",), None),
            (("--help=yes",), None),
            ((), "--interface and --keyboard are required"),
            (("--interface", "3"), "--interface and --keyboard are required"),
            (("--keyboard", "/dev/input/event0"), "--interface and --keyboard are required"),
        ],
        ids=[
            "unknown",
            "unexpected",
            "timeout-ms",
            "help-yes",
            "empty",
            "interface-3",
            "keyboard-dev-input-event0",
        ],
    )
    def test_unknown_positional_and_missing_required_arguments_are_errors(
        self, arguments: tuple[str, ...], message: str | None
    ) -> None:
        """Malformed and incomplete forwarding commands fail before device access."""
        self.assert_syntax_error(*arguments, message=message)

    def test_self_test_accepts_native_value_forms_prefixes_and_repeats(self) -> None:
        """Supported numeric syntax, long prefixes and repeated options remain valid."""
        result = self.run_keyboard(
            "--v=0525",
            "--vid",
            "0x0525",
            "--p",
            "a4a6",
            "--bus=010",
            "--b",
            "0x08",
            "--address",
            "9",
            "--address=0x09",
            "--t=0xfa",
            "--timeout-ms",
            "250",
            "--w",
            "00",
            "--no-d",
            "--no-detach",
            "--self",
            "--self-test",
        )

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == ("SELFTEST OK\n")
        assert (result.stderr) == ("")

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (("--vid=10000",), "--vid must be a 16-bit hexadecimal value"),
            (("--pid", "-1"), "--pid must be a 16-bit hexadecimal value"),
            (("--interface", "256"), "--interface must be in 0..255"),
            (("--bus", "256", "--address", "1"), "--bus must be in 0..255"),
            (("--bus", "1", "--address=0x100"), "--address must be in 0..255"),
            (("--timeout-ms", "0"), "--timeout-ms must be in 1..60000"),
            (("--wait=3601",), "--wait must be in 0..3600"),
            (("--interface", " -18446744073709551615"), "--interface must be in 0..255"),
            (("--vid", "gg", "--vid=0525"), "--vid must be a 16-bit hexadecimal value"),
            (("--wait", "bad", "--wait", "0"), "--wait must be in 0..3600"),
        ],
        ids=[
            "vid-10000",
            "pid-negative-one",
            "interface-256",
            "bus-256-address-1",
            "bus-1-address-0x100",
            "timeout-ms-0",
            "wait-3601",
            "interface-negative-one8446744073709551615",
            "vid-gg-vid-0525",
            "wait-bad-wait-0",
        ],
    )
    def test_every_supplied_numeric_occurrence_is_validated(
        self, arguments: tuple[str, ...], message: str
    ) -> None:
        """An invalid earlier scalar cannot be hidden by a later valid value."""
        self.assert_syntax_error(*arguments, "--self-test", message=message)

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (("--self-test", "--bus", "1"), "--bus and --address must be used together"),
            (("--self-test", "--address", "1"), "--bus and --address must be used together"),
            (
                ("--self-test", "--interface", "1"),
                "--list and --self-test cannot be combined with forwarding options",
            ),
        ],
        ids=["self-test-bus-1", "self-test-address-1", "self-test-interface-1"],
    )
    def test_self_test_keeps_non_forwarding_constraints(
        self, arguments: tuple[str, ...], message: str
    ) -> None:
        """Self-test bypasses required forwarding options but not combination rules."""
        result = self.run_keyboard("--self-test")
        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == ("SELFTEST OK\n")
        assert (result.stderr) == ("")

        self.assert_syntax_error(*arguments, message=message)
