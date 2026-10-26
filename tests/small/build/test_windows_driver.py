# SPDX-License-Identifier: GPL-2.0-only
"""Windows build recipe portability and native embedder environment."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.build.host_tools import windows_driver
from fplinux_cli.build.host_tools.windows_driver import _build_driver

if TYPE_CHECKING:
    from pathlib import Path

    import pytest


def test_relocated_driver_build_recipe_runs_without_the_project_package(tmp_path: Path) -> None:
    """Run only argument parsing, without compilation or Windows execution."""
    recipe = tmp_path / "build.py"
    shutil.copyfile(windows_driver.__file__, recipe)
    result = subprocess.run(
        [sys.executable, "-I", str(recipe), "--help"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=True,
    )
    assert "--toolchain" in result.stdout
    assert "--source-date-epoch" in result.stdout


def test_native_embedder_receives_explicit_epoch_without_changing_host_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Replace compilation processes; verify the native embedder's environment contract."""
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "987654321")
    source = tmp_path / "source"
    (source / "libwdi").mkdir(parents=True)
    (source / "libwdi/libwdi.rc").write_text("FILEVERSION 1,5,1,788\n", encoding="cp1252")
    with mock.patch(
        "fplinux_cli.build.host_tools.windows_driver.subprocess.run",
        return_value=subprocess.CompletedProcess([], 0),
    ) as processes:
        _build_driver(
            toolchain=tmp_path / "compiler",
            source=source,
            output=tmp_path / "helper.exe",
            source_date_epoch=1234567890,
        )
    embedded = [
        call
        for call in processes.call_args_list
        if call.args[0] == [str(source / "libwdi/embedder"), "embedded.h"]
    ]
    assert len(embedded) == 1
    assert embedded[0].kwargs["env"]["SOURCE_DATE_EPOCH"] == "1234567890"
    assert os.environ["SOURCE_DATE_EPOCH"] == "987654321"
