# SPDX-License-Identifier: GPL-2.0-only
"""Host text editing model with controlled English and Russian language files."""

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def text_model(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Link the production editing model with controlled language files."""
    executable = tmp_path_factory.mktemp("text-model") / "text-model"
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
            str(ROOT / "lib/fplinux/fplinux-text-input.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-o",
            str(executable),
        ],
        name="compile text editing model",
        timeout=30,
        check=True,
    )
    return executable


def test_text_model_modes_editing_and_deadlines(text_model: Path) -> None:
    """Text, caret, language/case modes and held-key deadlines follow input."""
    run_process(
        [str(text_model), str(ROOT / "tests/host_tool/input/fixtures/input-languages")],
        name="run text editing model",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("scenario", ["overdue-digit-release", "overdue-phone-zero-release"])
def test_normal_release_resolves_overdue_hold_before_tick(text_model: Path, scenario: str) -> None:
    """A queued release preserves the held digit or phone prefix exactly once."""
    run_process(
        [str(text_model), str(ROOT / "tests/host_tool/input/fixtures/input-languages"), scenario],
        name=f"run text model {scenario}",
        timeout=10,
        check=True,
    )
