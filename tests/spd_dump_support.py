# SPDX-License-Identifier: GPL-2.0-only
"""Build a declared uploader recipe against a controlled libusb process boundary."""

from __future__ import annotations

import shutil
import tomllib
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import sha256_file

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

    import pytest

USB_PEER = ROOT / "tests/fixtures/spd-dump"
UPSTREAM = Path("/opt/quality/src/spreadtrum-flash")


@contextmanager
def build_uploader(tmp_path_factory: pytest.TempPathFactory, *, platform: str) -> Iterator[Path]:
    """Compile the actual patched source while replacing only its libusb boundary."""
    locked = tomllib.loads((ROOT / "sources.lock.toml").read_text())["spreadtrum_flash"]
    config = tomllib.loads((ROOT / f"platforms/{platform}/platform.toml").read_text())
    recipe = next(tool for tool in config["host"]["tools"] if tool["name"] == "spd_dump")
    with c_build_directory(tmp_path_factory, f"{platform}-uploader") as work:
        compatibility = work / "include/libusb-1.0"
        compatibility.mkdir(parents=True)
        shutil.copyfile(USB_PEER / "usb-compat/libusb.h", compatibility / "libusb.h")
        for member in recipe["members"]:
            source = UPSTREAM / member["path"]
            assert sha256_file(source) == locked["files"][member["digest_key"]]
            shutil.copyfile(source, work / source.name)
        for step in recipe["copies"]:
            destination = work / step["destination"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / step["source"], destination)
        for relative in recipe["patches"]:
            run_process(
                ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(ROOT / relative)],
                name=f"patch pinned {platform} RAM uploader",
                cwd=work,
                timeout=10,
                check=True,
            )
        executable = work / "spd_dump"
        compile_c(
            executable,
            [
                "-std=c99",
                "-O2",
                "-Wall",
                "-Wextra",
                "-pedantic",
                "-DUSE_LIBUSB=1",
                f"-I{work / 'include'}",
                str(work / "spd_dump.c"),
                str(USB_PEER / "usb-peer.c"),
            ],
            name=f"compile {platform} RAM uploader with USB process peer",
        )
        yield executable
