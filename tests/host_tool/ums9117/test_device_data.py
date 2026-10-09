# SPDX-License-Identifier: GPL-2.0-only
"""Linked host checks of supplemental initramfs transfer and Linux DT properties."""

from __future__ import annotations

import hashlib
import random
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.device_tree import exact_path_properties
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.device_data.inputs import FirmwareInput

from tests import ROOT
from tests.c_harness import compile_c
from tests.fdt import binary_tree
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def device_data_peer(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Link the production transfer with host libc and a guarded memory arena."""
    work = tmp_path_factory.mktemp("device-data-peer")
    includes = work / "include"
    includes.mkdir()
    (includes / "ums9117-common").symlink_to(ROOT / "platforms/ums9117/common")
    executable = work / "device-data"
    bootstrap = ROOT / "platforms/ums9117/bootstrap"
    compile_c(
        executable,
        [
            "-std=c99",
            "-pedantic",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            f"-I{bootstrap}",
            f"-I{includes}",
            str(ROOT / "tests/host_tool/ums9117/device-data.c"),
            str(bootstrap / "boot-session.c"),
            "-Wl,--gc-sections",
        ],
        name="compile device-data transfer host peer",
        check=True,
    )
    return executable


def _firmware(contents: bytes) -> FirmwareInput:
    return FirmwareInput(
        source="profile.bin",
        destination="fplinux/profile.bin",
        contents=contents,
        sha256=hashlib.sha256(contents).hexdigest(),
    )


def _tree() -> bytes:
    return binary_tree(
        [],
        [
            (
                "chosen",
                [
                    ("bootargs", b"init=/init\0"),
                    ("linux,initrd-start", b"\xe5" * 4),
                    ("linux,initrd-end", b"\xf6" * 4),
                ],
                [],
            )
        ],
    )


def _transfer(peer: Path, directory: Path, capsule: bytes | None) -> tuple[str, bytes, bytes]:
    (directory / "tree.dtb").write_bytes(_tree())
    if capsule is not None:
        (directory / "device-data.cpio").write_bytes(capsule)
    result = run_process(
        [str(peer)],
        name="run device-data transfer host peer",
        cwd=directory,
        timeout=10,
        check=True,
    )
    return (
        result.stdout.strip(),
        (directory / "result.dtb").read_bytes(),
        (directory / "arena.bin").read_bytes(),
    )


def test_missing_capsule_clears_initrd_properties_and_preserves_memory(
    tmp_path: Path, device_data_peer: Path
) -> None:
    status, tree, arena = _transfer(device_data_peer, tmp_path, None)

    assert status == "0 0"
    assert arena == b"n" * 4096
    assert exact_path_properties(tree, "/chosen")["/chosen"] == {
        "bootargs": b"init=/init\0",
        "linux,initrd-start": b"\0" * 4,
        "linux,initrd-end": b"\0" * 4,
    }


@pytest.mark.parametrize("profile", [None, b"audio profile\0"])
def test_capsule_is_copied_below_dtb_and_published_as_linux_initrd(
    tmp_path: Path, device_data_peer: Path, profile: bytes | None
) -> None:
    groups = {} if profile is None else {"audio-profile": (_firmware(profile),)}
    capsule = create_device_data_initramfs(groups, usb_transport="rndis")

    status, tree, arena = _transfer(device_data_peer, tmp_path, capsule)

    assert status == f"0 {len(capsule)}"
    assert arena == b"n" * (4096 - len(capsule)) + capsule
    properties = exact_path_properties(tree, "/chosen")["/chosen"]
    assert properties["bootargs"] == b"init=/init\0"
    assert properties["linux,initrd-start"] == (0x83E00000 - len(capsule)).to_bytes(4, "big")
    assert properties["linux,initrd-end"] == b"\x83\xe0\x00\x00"


def test_capsule_that_exceeds_arena_leaves_tree_and_memory_untouched(
    tmp_path: Path, device_data_peer: Path
) -> None:
    contents = random.Random(1).randbytes(4096)  # noqa: S311 -- fixed non-secret test input.
    capsule = create_device_data_initramfs({"audio-profile": (_firmware(contents),)})
    assert len(capsule) > 4096

    status, tree, arena = _transfer(device_data_peer, tmp_path, capsule)

    assert status == "1 0"
    assert tree == _tree()
    assert arena == b"n" * 4096
