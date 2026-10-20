# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the linked keypad LED driver through host subsystem boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e"


@pytest.fixture(scope="module")
def keypad_led(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Replace regmap and LED registration without replacing driver callbacks."""
    with c_build_directory(tmp_path_factory, "sc2721-kpled") as directory:
        executable = directory / "sc2721-kpled"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURES / "kpled-compat"),
                str(FIXTURES / "sc2721-kpled.c"),
                str(ROOT / "platforms/sc9820e/linux/overlay/drivers/leds/leds-sc2721-kpled.c"),
            ],
            name="link the SC2721 keypad LED driver with host subsystem boundaries",
        )
        yield executable


@pytest.mark.parametrize("scenario", ["brightness", "errors", "lifecycle"])
def test_keypad_led_callbacks_preserve_output_contracts(keypad_led: Path, scenario: str) -> None:
    """Check encoded brightness, partial-write OFF and lifecycle power-down."""
    run_process(
        [str(keypad_led), scenario],
        name=f"exercise linked SC2721 keypad LED callbacks: {scenario}",
        timeout=5,
        check=True,
    )
