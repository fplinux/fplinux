# SPDX-License-Identifier: GPL-2.0-only
"""Exercise patched bootstrap USB control handling with controlled registers."""

from __future__ import annotations

import tarfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

ARCHIVE = Path("/opt/quality/src/fpdoom-bootstrap.tar.gz")
PATCH = ROOT / "platforms/ums9117/bootstrap/patches/0006-usb-control-setup.patch"
TEST_DIR = ROOT / "tests/host_tool/ums9117/bootstrap"


@pytest.fixture(scope="module")
def usb_executable(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the patched bootstrap source as a separate translation unit."""
    with c_build_directory(tmp_path_factory, "bootstrap-usb-control") as directory:
        with tarfile.open(ARCHIVE, "r:gz") as archive:
            for name in ("ums9117/usbio.c", "fpdoom/usbio.h"):
                members = [m for m in archive.getmembers() if m.name.endswith(f"/{name}")]
                assert len(members) == 1
                source = archive.extractfile(members[0])
                assert source is not None
                destination = directory / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                with source:
                    destination.write_bytes(source.read())
        run_process(
            ["patch", "--batch", "--fuzz=0", "-p1", "-i", str(PATCH)],
            name="apply bootstrap USB patch",
            cwd=directory,
            timeout=10,
            check=True,
        )
        executable = directory / "usb-control"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                # Keep the register peer independent of host stack contents.
                "-ftrivial-auto-var-init=zero",
                "-Wno-pointer-to-int-cast",
                f"-I{TEST_DIR / 'usb-control-compat'}",
                f"-I{directory / 'fpdoom'}",
                str(directory / "ums9117/usbio.c"),
                str(TEST_DIR / "usb-control.c"),
            ],
            name="compile bootstrap USB register peer",
            check=True,
        )
        yield executable


@pytest.mark.parametrize(
    ("descriptor", "length", "expected"),
    [
        (2, 9, "09022000010100c032"),
        (2, 32, "09022000010100c0320904000002ff0000000705850200020007050602000200"),
        (1, 18, "12010002000000408217004dffff00000001"),
    ],
    ids=["configuration-header", "full-configuration", "device"],
)
def test_get_descriptor_returns_requested_bytes(
    usb_executable: Path, descriptor: int, length: int, expected: str
) -> None:
    result = run_process(
        [str(usb_executable), str(descriptor), str(length)],
        name="request bootstrap USB descriptor",
        timeout=5,
        check=True,
    )
    status, control, accesses, data = result.stdout.split()
    assert int(status) == -1
    assert int(control, 16) & 0x0A == 0x0A
    assert int(accesses) == 8 + length
    assert bytes.fromhex(data) == bytes.fromhex(expected)


def test_empty_nonblocking_read_returns_without_changing_buffer(usb_executable: Path) -> None:
    result = run_process(
        [str(usb_executable), "empty"],
        name="read empty bootstrap USB receive queue",
        timeout=5,
        check=True,
    )
    assert result.stdout.strip() == "0 aabbccdd"
