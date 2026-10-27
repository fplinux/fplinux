# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for language definitions and the shared reader."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_language_reader_loads_shipped_files_and_rejects_invalid_definitions(
    tmp_path: Path,
) -> None:
    executable = tmp_path / "fplinux-language"
    shipped = tmp_path / "shipped"
    scratch = tmp_path / "scratch"
    shipped.mkdir()
    scratch.mkdir()
    for code in ("en", "ru"):
        shutil.copyfile(ROOT / "alpine/aports/fplinux-language" / code, shipped / code)
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
        ],
        name="compile language reader harness",
    )
    result = run_process(
        [str(executable), str(shipped), str(scratch)],
        name="read language definitions and exercise validation",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-language: OK\n"


def test_contextual_commands_translate_and_keep_plain_english_fallback(tmp_path: Path) -> None:
    """The actual gettext catalogue distinguishes full and short command labels."""
    executable = tmp_path / "gettext-context"
    catalogue = tmp_path / "ru/LC_MESSAGES/gettext-context.mo"
    catalogue.parent.mkdir(parents=True)
    run_process(
        [
            "msgfmt",
            "--check",
            "--output-file",
            str(catalogue),
            str(ROOT / "alpine/aports/fplinux-notes/ru.po"),
        ],
        name="compile contextual command catalogue",
        timeout=10,
        check=True,
    )
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/gettext-context.c"),
            *gettext_link_flags(),
        ],
        name="compile contextual gettext consumer",
    )
    for locale, expected_lines in [
        ("ru_RU.UTF-8", ["Повтор", "Повторить", "Retry", "Готово"]),
        ("en_US.UTF-8", ["Retry", "Retry", "Retry", "Done"]),
    ]:
        result = run_process(
            [str(executable), str(tmp_path)],
            name=f"translate contextual commands with {locale} fallback",
            env={"LC_ALL": locale},
            timeout=10,
            check=True,
        )
        assert result.stdout == "\n".join(expected_lines) + "\n"


@pytest.mark.parametrize("package", ["fplinux-clocks", "fplinux-files", "fplinux-mre"])
def test_refreshing_application_catalogue_preserves_complete_russian_messages(
    tmp_path: Path, package: str
) -> None:
    """The real gettext extraction/merge/compile workflow retains current translations."""
    aport = ROOT / "alpine/aports" / package
    template = tmp_path / "messages.pot"
    merged = tmp_path / "ru.po"
    run_process(
        [
            "xgettext",
            "--from-code=UTF-8",
            "--language=C",
            "--keyword=_",
            "--keyword=N_",
            "--keyword=NC_:1c,2",
            "--output",
            str(template),
            *(str(path) for path in sorted(aport.rglob("*.c"))),
        ],
        name=f"extract current {package} messages",
        timeout=10,
        check=True,
    )
    run_process(
        [
            "msgmerge",
            "--no-fuzzy-matching",
            "--output-file",
            str(merged),
            str(aport / "ru.po"),
            str(template),
        ],
        name=f"refresh {package} Russian messages in scratch",
        timeout=10,
        check=True,
    )
    result = run_process(
        [
            "msgfmt",
            "--check",
            "--statistics",
            "--output-file",
            str(tmp_path / "ru.mo"),
            str(merged),
        ],
        name=f"compile refreshed {package} Russian catalogue",
        env={"LC_ALL": "C"},
        timeout=10,
        check=True,
    )
    assert "untranslated" not in result.stderr, result.stderr
    assert "fuzzy" not in result.stderr, result.stderr
