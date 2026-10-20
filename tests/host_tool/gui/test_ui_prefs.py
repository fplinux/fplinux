# SPDX-License-Identifier: GPL-2.0-only
"""Host preferences file format, atomic replacement and sync failures."""

from __future__ import annotations

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


def test_preferences_round_trip_validation_and_durable_save(tmp_path: Path) -> None:
    executable = tmp_path / "preferences"
    state = tmp_path / "state"
    state.mkdir()
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name("fplinux-ui-prefs.c")),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            "-Wl,--wrap=fsync",
            "-o",
            str(executable),
        ],
        name="compile UI preferences harness",
        timeout=30,
        check=True,
    )
    result = run_process(
        [str(executable), str(state)],
        name="run UI preferences harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"
    reported = [
        int(line.rsplit(":", 2)[1])
        for line in result.stderr.splitlines()
        if line.endswith("malformed preference ignored")
    ]
    assert reported == [2, 3, 4, 5, 6, 7, 11]
