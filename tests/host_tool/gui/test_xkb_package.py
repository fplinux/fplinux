# SPDX-License-Identifier: GPL-2.0-only
"""Exercise installed keyboard data through the compositor's XKB rules boundary."""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

SYSTEM_XKB = Path("/usr/share/X11/xkb")


def test_packaged_rules_compile_us_and_ru_with_layout_switching(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Actual package functions install a self-contained rules-based keymap.

    The pinned build image supplies upstream layout inputs. This executes the
    package functions and libxkbcommon on the host, without building APK archives
    or starting the compositor.
    """
    with c_build_directory(tmp_path_factory, "xkb-package") as directory:
        builddir = directory / "upstream"
        for component in ("keycodes", "types", "compat", "symbols"):
            shutil.copytree(SYSTEM_XKB / component, builddir / component)
        (builddir / "COPYING").write_text("Test package license input.\n")
        pkgdir = directory / "installed"
        for package in ("fplinux-xkb", "fplinux-xkb-ru"):
            aport = ROOT / "alpine/aports" / package
            run_process(
                [
                    "sh",
                    "-eu",
                    "-c",
                    'srcdir=$3; . "$1"; builddir=$2; pkgdir=$4; package',
                    "package-xkb",
                    str(aport / "APKBUILD"),
                    str(builddir),
                    str(aport),
                    str(pkgdir),
                ],
                name=f"install {package} through package function",
                timeout=10,
                check=True,
            )
        executable = directory / "xkb-package"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(Path(__file__).with_name("fplinux-xkb-package.c")),
                "-lxkbcommon",
            ],
            name="compile packaged XKB rules consumer",
            check=True,
        )
        run_process(
            [str(executable), str(pkgdir / "usr/share/fplinux/xkb")],
            name="compile and use installed XKB rules",
            timeout=10,
            check=True,
        )
