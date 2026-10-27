# SPDX-License-Identifier: GPL-2.0-only
"""Pinned native inputs shared by real font, widget and application consumers."""

from __future__ import annotations

import os
import sys
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="session")
def lvgl_library(tmp_path_factory: pytest.TempPathFactory) -> Iterator[tuple[Path, Path]]:
    """Build the real library once with the phone configuration and source fixes."""
    with c_build_directory(tmp_path_factory, "ui-lvgl") as directory:
        run_process(
            [
                "sh",
                str(ROOT / "scripts/build-ui-lvgl.sh"),
                str(directory),
            ],
            name="build the shared LVGL font and widget runtime",
            timeout=180,
            check=True,
        )
        yield directory / "source/include", directory / "build/liblvgl.a"


@pytest.fixture(scope="session")
def ui_host_inputs(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Iterator[Path]:
    """Prepare the UI font set and xdg-shell once beside the shared LVGL library."""
    headers, archive = lvgl_library
    with c_build_directory(tmp_path_factory, "ui-host-inputs") as directory:
        run_process(
            [
                "sh",
                str(ROOT / "site/scripts/screens/ui-host-build.sh"),
                str(directory),
                str(headers),
                str(archive),
            ],
            name="prepare the shared native application fonts and Wayland protocol",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.fixture(scope="session")
def ui_icon_resources(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Generate the installed glyph tree from the pinned production icon inputs."""
    with c_build_directory(tmp_path_factory, "ui-icon-resources") as directory:
        archives = list(Path("/opt/quality/src/ui-icons").iterdir())
        assert len(archives) == 1
        sources = directory / "sources"
        with tarfile.open(archives[0]) as archive:
            archive.extractall(sources, filter="data")
        installed = directory / "icons"
        run_process(
            [
                sys.executable,
                str(ROOT / "alpine/aports/fplinux-icons/generate-icons.py"),
                "--tabler",
                str(sources / "package/icons"),
                "--profile",
                str(ROOT / "lib/fplinux/fplinux-ui-profile.json"),
                "--out",
                str(installed),
            ],
            name="generate the actual pinned system glyphs for native UI consumers",
            timeout=60,
            env=os.environ | {"NODE_PATH": "/opt/quality/node-tools/node_modules"},
            check=True,
        )
        yield installed
