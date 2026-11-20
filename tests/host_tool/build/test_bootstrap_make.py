# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the generated settings and real bootstrap Make safety boundary."""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.build.bootstrap import recipe as bootstrap_build

from tests import ROOT
from tests.process import run_process


@pytest.mark.parametrize(
    ("mode", "width", "loader_display"),
    [
        ("linux", 0, False),
        ("linux", 128, True),
        ("uboot", 128, True),
        ("linux", 240, False),
        ("uboot", 240, False),
    ],
    ids=["ram-headless", "ram-display", "uboot-display", "linux-only-display", "uboot-headless"],
)
def test_bootstrap_make_selects_display_objects_and_blocks_storage_overrides(
    tmp_path: Path, mode: str, width: int, *, loader_display: bool
) -> None:
    """Generated settings select the Make closure even against command-line overrides."""
    bootstrap = tmp_path / "bootstrap"
    generated = bootstrap / "generated"
    generated.mkdir(parents=True)
    vendor = tmp_path / "vendor/fpdoom"
    vendor.mkdir(parents=True)
    (vendor / "build_t117.make").write_text("")
    config = {
        "adapter": {"spi_mode": 1} if loader_display else {},
        "display": {"width": width},
        "bootstrap": {
            "kind": "uboot-stage0" if mode == "uboot" else "linux",
            "image": "test.bin",
            "map": "obj/test_map.txt",
            "dtb_destination": "test.dtb",
        },
    }
    (generated / "fplinux-bootstrap-settings.make").write_bytes(
        bootstrap_build.bootstrap_settings(config)
    )
    for name in (
        "fplinux-bootstrap-identity.h",
        "fplinux-boot-layout.h",
        "fplinux-bootstrap-display.h",
        "fplinux-bootstrap-memory.ld",
        "fplinux-uboot-build.h",
    ):
        (generated / name).touch()
    makefile = bootstrap / "Makefile"
    makefile.write_text(
        "include generated/fplinux-bootstrap-settings.make\n"
        f"include {ROOT / 'platforms/ums9117/bootstrap/build/bootstrap.make'}\n"
    )
    result = run_process(
        [
            "make",
            "-s",
            "fplinux-safety-check",
            "LIBC_SDIO=1",
            "APP_SRCS=unsafe",
            "FPLINUX_STATUS_SRCS=unsafe",
            f"FPLINUX_BOOTSTRAP_HAS_DISPLAY={int(not loader_display)}",
        ],
        name="inspect protected bootstrap source closure",
        cwd=bootstrap,
        timeout=10,
        check=True,
    )
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines())
    objects = set(fields["OBJS"].split())
    assert fields["LIBC_SDIO"] == "0"
    assert "obj/sys/sdio.o" not in objects
    assert "obj/sys/microfat.o" not in objects
    assert "obj/app/unsafe.o" not in objects
    assert ("obj/app/fplinux-boot-screen/boot-screen.o" in objects) == loader_display
    assert ("obj/app/fplinux-status-screen.o" in objects) == loader_display
    assert ("obj/app/generated/fplinux-status-fonts.o" in objects) == loader_display
    stages = {Path(path).name for path in objects} & {"sd-stage0.o", "boot-main.o"}
    assert stages == ({"sd-stage0.o"} if mode == "uboot" else {"boot-main.o"})
