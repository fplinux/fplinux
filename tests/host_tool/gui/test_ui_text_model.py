# SPDX-License-Identifier: GPL-2.0-only
"""Host text editing model with controlled English and Russian language files."""

from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


def test_text_model_modes_editing_and_deadlines(tmp_path: Path) -> None:
    """Text, caret, language/case modes and held-key deadlines follow input."""
    executable = tmp_path / "text-model"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(ROOT / "tests/host_tool/gui/fplinux-ui-text-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-text-model.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            "-o",
            str(executable),
        ],
        name="compile text editing model",
        timeout=30,
        check=True,
    )
    run_process(
        [str(executable), str(ROOT / "tests/host_tool/input/fixtures/input-languages")],
        name="run text editing model",
        timeout=10,
        check=True,
    )
