# SPDX-License-Identifier: GPL-2.0-only
"""Pinned native inputs shared by real font, widget and application consumers."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


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
    """Prepare the full Noto set and xdg-shell once beside the shared LVGL library."""
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
