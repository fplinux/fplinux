# SPDX-License-Identifier: GPL-2.0-only
"""The pinned LVGL runtime shared by real font and widget consumers."""

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
        source = directory / "source"
        source.mkdir()
        run_process(
            [
                "tar",
                "-xzf",
                "/opt/quality/src/lvgl.tar.gz",
                "--strip-components=1",
                "-C",
                str(source),
            ],
            name="extract the pinned LVGL source",
            timeout=30,
            check=True,
        )
        aport = ROOT / "alpine/aports/fplinux-lvgl"
        for patch in ("shared-abi.patch", "canvas-cache-drop.patch"):
            run_process(
                ["patch", "-p1", "-d", str(source), "-i", str(aport / patch)],
                name=f"prepare LVGL {patch}",
                timeout=10,
                check=True,
            )
        build = directory / "build"
        run_process(
            [
                "cmake",
                "-S",
                str(source),
                "-B",
                str(build),
                "-G",
                "Ninja",
                "-DCMAKE_C_COMPILER=cc",
                "-DBUILD_SHARED_LIBS=OFF",
                f"-DLV_BUILD_CONF_PATH={aport / 'lv_conf.h'}",
                "-DLV_BUILD_SET_CONFIG_OPTS=ON",
                "-DLV_BUILD_LVGL_H_SYSTEM_INCLUDE=ON",
                "-DLV_BUILD_INSTALL=OFF",
                "-DLV_FETCH_DEPENDENCIES=OFF",
                "-DLV_USE_FIND_PACKAGE=OFF",
                "-DLV_USE_PKG_CONFIG=OFF",
            ],
            name="configure LVGL with the current application configuration",
            timeout=30,
            check=True,
        )
        run_process(
            ["cmake", "--build", str(build), "--target", "lvgl", "--parallel", "2"],
            name="build the shared LVGL font and widget runtime",
            timeout=180,
            check=True,
        )
        yield source / "include", build / "liblvgl.a"
