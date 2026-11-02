# SPDX-License-Identifier: GPL-2.0-only
"""Shared runtime settings through real preference files."""

import shlex
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_runtime_settings_recent_documents_and_durable_save(
    tmp_path: Path, lvgl_library: tuple[Path, Path]
) -> None:
    headers, _ = lvgl_library
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve runtime preferences JSON dependency",
        timeout=10,
        check=True,
    )
    executable = tmp_path / "runtime-preferences"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/ui/runtime-preferences.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-recent.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-view-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-Wl,--gc-sections",
            "-Wl,--wrap=fsync",
            *shlex.split(flags.stdout),
        ],
        name="link runtime preference behavior harness",
    )
    result = run_process(
        [str(executable), str(tmp_path / "preferences")],
        name="runtime preference persistence",
        timeout=10,
        check=True,
    )
    assert result.stdout == "runtime-preferences: ok\n"
