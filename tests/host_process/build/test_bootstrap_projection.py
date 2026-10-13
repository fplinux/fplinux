# SPDX-License-Identifier: GPL-2.0-only
"""Host consumers of generated bootstrap names, font and geometry."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.bootstrap.recipe import bootstrap_display_header, bootstrap_settings

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(("kind", "mode"), [("linux", "linux"), ("uboot-stage0", "uboot")])
def test_make_consumer_receives_selected_names_mode_and_font(
    tmp_path: Path, kind: str, mode: str
) -> None:
    """A synthetic Make consumer reads the selected settings for either bootstrap kind."""
    target = {
        "bootstrap": {
            "kind": kind,
            "image": "selected_stage.bin",
            "map": "obj/selected.map",
            "dtb_destination": "phone.dtb",
        },
        "display": {"boot_font": "7x14"},
    }
    (tmp_path / "settings.make").write_bytes(bootstrap_settings(target))
    consumer = tmp_path / "consumer.make"
    shutil.copyfile(ROOT / "tests/fixtures/bootstrap-projection/consumer.make", consumer)
    result = run_process(
        ["make", "--no-print-directory", "-f", str(consumer)],
        cwd=tmp_path,
        name="read generated bootstrap settings with Make",
        timeout=10,
        check=True,
    )
    assert result.stdout.splitlines() == [
        "selected_stage",
        "obj/selected.map",
        "phone.dtb",
        mode,
        "7x14",
    ]


@pytest.mark.parametrize(("width", "height"), [(0, 0), (128, 160), (320, 240)])
def test_target_main_passes_selected_geometry_to_the_boot_runtime(
    tmp_path: Path, width: int, height: int
) -> None:
    """Link the target main to a host runtime stub using the actual bootstrap headers."""
    generated = tmp_path / "generated"
    generated.mkdir()
    (generated / "fplinux-bootstrap-display.h").write_bytes(
        bootstrap_display_header({"width": width, "height": height})
    )
    runtime = tmp_path / "ums9117-bootstrap"
    runtime.mkdir()
    shutil.copy2(ROOT / "platforms/ums9117/bootstrap/src/boot-main.h", runtime / "boot-main.h")
    stub = tmp_path / "runtime.c"
    shutil.copyfile(ROOT / "tests/fixtures/bootstrap-projection/runtime.c", stub)
    executable = tmp_path / "geometry"
    run_process(
        [
            "cc",
            "-std=c99",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(ROOT / "bootstrap"),
            str(ROOT / "platforms/ums9117/target-template/bootstrap/main.c"),
            str(stub),
            "-o",
            str(executable),
        ],
        name="compile target geometry against bootstrap headers",
        timeout=15,
        check=True,
    )
    result = run_process(
        [str(executable)], name="observe target board geometry", timeout=5, check=True
    )
    assert result.stdout == f"{width} {height}\n"
