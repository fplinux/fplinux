# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for Settings/manager values and clock edits."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_settings_values_and_date_only_save_preserves_running_clock(tmp_path: Path) -> None:
    """The linked core formats literal values and uses Save's current seconds."""
    aport = ROOT / "alpine/aports/fplinux-settings"
    session = ROOT / "alpine/aports/fplinux-session"
    executable = tmp_path / "settings-core"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(aport),
            "-I",
            str(session),
            "-I",
            str(ROOT / "include/fplinux"),
            str(ROOT / "tests/host_tool/gui/settings_core.c"),
            str(aport / "settings-core.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(session / "session-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        ],
        name="compile Settings core harness",
    )
    os_release = tmp_path / "os-release"
    os_release.write_text('NAME="FPLinux"\nVERSION_ID=1\nBUILD_ID="012345abcdef"\n')
    result = run_process(
        [str(executable), str(os_release)],
        name="run Settings core harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "settings-core: all checks passed\n"
