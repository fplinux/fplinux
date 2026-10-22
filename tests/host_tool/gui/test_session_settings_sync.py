# SPDX-License-Identifier: GPL-2.0-only
"""Settings publication with controlled file and directory sync failures."""

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


@pytest.fixture
def settings_sync_harness(tmp_path: Path) -> Path:
    executable = tmp_path / "session-settings-sync-test"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            "-I",
            str(APORT),
            str(Path(__file__).with_name("fplinux-session-settings-sync-test.c")),
            str(APORT / "session-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-Wl,--wrap=fsync",
            "-o",
            str(executable),
        ],
        name="compile settings durability host harness",
        timeout=30,
        check=True,
    )
    return executable


@pytest.mark.parametrize("failure", ["0", "1", "2"], ids=["success", "file", "directory"])
def test_settings_save_reports_durability_result(
    settings_sync_harness: Path, failure: str
) -> None:
    run_process(
        [str(settings_sync_harness), failure],
        name="run settings durability host harness",
        timeout=10,
        check=True,
    )
