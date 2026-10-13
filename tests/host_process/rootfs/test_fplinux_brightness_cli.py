# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for the shipped brightness daemon and command."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests.fixtures.rootfs.brightness import (
    brightness as brightness,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.rootfs.brightness import (
    brightness_binaries as brightness_binaries,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.fixtures.rootfs.brightness import BrightnessProcesses

LEVELS = (0, 1, 2, 4, 7, 11, 16, 23, 32, 44, 63)


class FPLinuxBrightnessCliTests:
    """Check the brightness command's observable host behavior."""

    @pytest.mark.parametrize(
        ("level", "raw"),
        list(enumerate(LEVELS)),
        ids=[
            "level-0",
            "level-1",
            "level-2",
            "level-3",
            "level-4",
            "level-5",
            "level-6",
            "level-7",
            "level-8",
            "level-9",
            "level-10",
        ],
    )
    def test_cli_sets_all_logical_levels_and_gets_desired_level(
        self, brightness: BrightnessProcesses, level: int, raw: int
    ) -> None:
        """Every logical level writes the specified raw code and is reported by get."""
        brightness.start_daemon()
        assert (brightness.run_cli("get").stdout) == ("7\n")
        assert (brightness.raw()) == ("23\n")
        if level:
            assert brightness.run_cli("set", str(level - 1)).returncode == 0
        assert (brightness.run_cli("set", str(level)).returncode) == (0)
        assert (brightness.raw()) == (f"{raw}\n")
        assert (brightness.run_cli("get").stdout) == (f"{level}\n")

    @pytest.mark.parametrize(
        "args",
        [(), ("set",), ("set", "01"), ("set", "11"), ("set", "1.0")],
        ids=["missing-operation", "missing-level", "leading-zero", "out-of-range", "fractional"],
    )
    def test_cli_help_and_invalid_levels_need_no_service(
        self, brightness: BrightnessProcesses, args: tuple[str, ...]
    ) -> None:
        """Help and invalid arguments resolve before opening the socket."""
        help_result = brightness.run_cli("set", "--help")
        assert (help_result.returncode) == (0)
        assert ("LEVEL") in (help_result.stdout)
        assert (brightness.run_cli(*args).returncode) == (2)

    def test_cli_reports_unavailable_service(self, brightness: BrightnessProcesses) -> None:
        """Without a listening daemon the command fails with a service diagnostic."""
        result = brightness.run_cli("set", "8")
        assert (result.returncode) == (1)
        assert ("service unavailable") in (result.stderr)
