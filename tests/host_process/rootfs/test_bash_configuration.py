# SPDX-License-Identifier: GPL-2.0-only
"""Observe the Bash recipe's cross-configure environment without building Bash."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path

RECIPE = ROOT / "alpine/aports/fplinux-bash/APKBUILD"
FIXTURES = ROOT / "tests/host_process/rootfs/fixtures/bash-configure"


def test_cross_build_configures_musl_allocating_getcwd(tmp_path: Path) -> None:
    """The musl getcwd(NULL, 0) interface must survive cross configuration."""
    environment = os.environ | {
        "CBUILD": "x86_64-alpine-linux-musl",
        "CHOST": "armv7-alpine-linux-musleabihf",
        "CBUILDROOT": str(tmp_path / "sysroot"),
        "CFLAGS": "",
        "CXXFLAGS": "",
        "LDFLAGS": "",
    }
    environment.pop("bash_cv_getcwd_malloc", None)

    result = subprocess.run(
        [
            "sh",
            str(FIXTURES / "build-recipe.sh"),
            str(RECIPE),
            str(tmp_path / "src"),
            str(FIXTURES / "configure-boundary.sh"),
        ],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stderr == ""
    assert result.stdout == "yes\n"
