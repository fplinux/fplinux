# SPDX-License-Identifier: GPL-2.0-only
"""Vibration policy timelines and evdev behavior with a controlled UAPI boundary."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


@pytest.fixture(scope="module")
def vibration_harnesses(tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Path]]:
    with c_build_directory(tmp_path_factory, "session-vibration") as directory:
        executables = {}
        for component in ("vibration", "vibrator"):
            executable = directory / component
            wrappers = []
            if component == "vibrator":
                wrappers = [
                    f"-Wl,--wrap={function}"
                    for function in ("open", "fstat", "ioctl", "write", "close")
                ]
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    "-I",
                    str(APORT),
                    str(Path(__file__).with_name(f"session-{component}-test.c")),
                    str(APORT / f"session-{component}.c"),
                    *wrappers,
                ],
                name=f"compile session {component} host harness",
            )
            executables[component] = executable
        yield executables


def test_vibration_policy_timelines(vibration_harnesses: dict[str, Path]) -> None:
    run_process(
        [str(vibration_harnesses["vibration"])],
        name="run vibration patterns, lifecycle and limit timelines",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    "scenario",
    [
        "absent",
        "noncharacter",
        "wrong-phys",
        "no-ff",
        "no-rumble",
        "stat-fails",
        "ambiguous",
        "upload-fails",
        "zero-duration",
        "unique",
        "clock-effect",
        "start-fails",
        "short-write",
        "interrupted-write",
        "stop-fails",
        "erase-fails",
        "close-fails",
        "close-stop-fails",
    ],
)
def test_vibrator_evdev_contract(
    vibration_harnesses: dict[str, Path], tmp_path: Path, scenario: str
) -> None:
    count = {"absent": 0, "ambiguous": 2, "unique": 3}.get(scenario, 1)
    for index in range(count):
        (tmp_path / f"event{index}").touch()
    for ignored in ("event", "event1x", "unrelated"):
        (tmp_path / ignored).touch()
    run_process(
        [str(vibration_harnesses["vibrator"]), scenario, str(tmp_path)],
        name=f"run session vibrator {scenario}",
        timeout=10,
        check=True,
    )
