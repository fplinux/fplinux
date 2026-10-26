# SPDX-License-Identifier: GPL-2.0-only
"""Consume the generated WinUSB package metadata in a native C program."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.host_tools.windows_driver import _configure_driver

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("resource", "epoch", "expected"),
    [
        (b"FILEVERSION 1,5,1,788\r\n", 1234567890, "02/13/2009\n1.5.1.788\n"),
        (
            b"// Copyright \xa9\r\n FILEVERSION 2,7,0,42\r\n",
            1704067200,
            "01/01/2024\n2.7.0.42\n",
        ),
    ],
)
def test_driver_metadata_tracks_source_version_and_fixed_utc_epoch(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    resource: bytes,
    epoch: int,
    expected: str,
) -> None:
    """Build the header's real C consumer; no Windows executable or installer runs."""
    monkeypatch.setenv("TZ", "Pacific/Honolulu")
    (tmp_path / "libwdi").mkdir()
    (tmp_path / "libwdi/libwdi.rc").write_bytes(resource)

    _configure_driver(tmp_path, source_date_epoch=epoch)
    executable = tmp_path / "metadata"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(tmp_path),
            str(ROOT / "tests/host_tool/build/fixtures/windows-driver-metadata.c"),
        ],
        name="compile generated WinUSB metadata consumer",
    )
    result = run_process(
        [str(executable)], name="read generated WinUSB metadata", timeout=5, check=True
    )

    assert result.stdout == expected
