# SPDX-License-Identifier: GPL-2.0-only
"""Windows redistributable projection with a controlled archive-extractor boundary."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.build import windows_driver
from fplinux_cli.build.windows_driver import _build_driver, _extract_redist


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


@pytest.mark.parametrize("listed", [True, False])
def test_redist_projection_requires_both_files_to_be_redistributable(
    tmp_path: Path, *, listed: bool
) -> None:
    """Replace only 7-Zip extraction; file selection and license checks remain real."""
    source = tmp_path / "source"
    extracted = source / "wdf-redistributables"
    extracted.mkdir(parents=True)
    (extracted / "_WdfCoInstaller01011.dll_00001").write_bytes(b"x64 framework")
    (extracted / "_winusbcoinstaller2.dll_00001").write_bytes(b"x64 winusb")
    (extracted / "_WdfCoInstaller01011.dll_00000").write_bytes(b"x86 framework")
    entries = "Redist\\wdf\\x64\\WdfCoInstaller01011.dll\n"
    if listed:
        entries += "Redist\\wdf\\x64\\winusbcoinstaller2.dll\n"
    (extracted / "redist.txt.x64").write_text(entries, encoding="utf-8")
    with mock.patch(
        "fplinux_cli.build.windows_driver.subprocess.run",
        return_value=subprocess.CompletedProcess([], 0),
    ):
        if not listed:
            with pytest.raises(ValueError, match="not listed for redistribution: winusb"):
                _extract_redist(tmp_path / "input.msi", source)
            assert not (source / "redist/wdf/x64/winusbcoinstaller2.dll").exists()
            return
        result = _extract_redist(tmp_path / "input.msi", source)
    assert result == extracted
    assert (source / "redist/wdf/x64/WdfCoInstaller01011.dll").read_bytes() == b"x64 framework"
    assert (source / "redist/wdf/x64/winusbcoinstaller2.dll").read_bytes() == b"x64 winusb"
    assert not (source / "redist/wdf/x86").exists()


def test_native_embedder_receives_explicit_epoch_without_changing_host_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Replace compilation processes; verify the native embedder's environment contract."""
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "987654321")
    source = tmp_path / "source"
    source.mkdir()
    with mock.patch(
        "fplinux_cli.build.windows_driver.subprocess.run",
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
    expected = Path(__file__).with_name("fixtures") / "windows-driver-config.h.fixture"
    assert (source / "config.h").read_bytes() == expected.read_bytes()
