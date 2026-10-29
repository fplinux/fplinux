# SPDX-License-Identifier: GPL-2.0-only
"""Public argument diagnostics for Alpine checksum selection."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_canonical_aport_directory_suggests_its_package_name(tmp_path: Path) -> None:
    result = run_process(
        [str(ROOT / "fplinux"), "checksum", "alpine/aports/fplinux-ui"],
        name="checksum selector hint",
        cwd=ROOT,
        env=os.environ | {"FPLINUX_DATA_DIR": str(tmp_path / "data")},
        timeout=10,
    )

    assert result.returncode == 1
    assert "use the package name: ./fplinux checksum fplinux-ui" in result.stderr
    assert "Traceback" not in result.stderr
