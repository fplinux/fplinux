# SPDX-License-Identifier: GPL-2.0-only
"""Validate independent UMS RAM and resident stage0 image examples."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.bootstrap.verify import verify_images, verify_sd_stage0_image
from fplinux_cli.build.storage.uboot import UbootBuild

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class UmsImage:
    """Test-owned payload bytes, linker symbols and the selected public verifier."""

    image: Path
    kernel: Path
    dtb: Path
    map_file: Path
    symbols: dict[str, int]
    uboot: UbootBuild | None
    slot: int

    def write_symbols(self) -> None:
        """Publish the test's symbol values in the normal linker-map input format."""
        self.map_file.write_text(
            "".join(f"{value:08x} T {name}\n" for name, value in self.symbols.items())
        )

    def verify(self, *, payload_limit: int = 0x80200000) -> dict[str, int | str]:
        """Exercise the image entry point used by the corresponding build mode."""
        if self.uboot is not None:
            return verify_sd_stage0_image(
                self.image,
                self.uboot,
                map_file=self.map_file,
                load_address=0x80100000,
                payload_limit=payload_limit,
                layout={"uboot_stack": 0x80110000},
            )
        return verify_images(
            self.image,
            zimage=self.kernel,
            dtb=self.dtb,
            map_file=self.map_file,
            load_address=0x80100000,
            payload_limit=payload_limit,
            forbidden_markers=[],
        )


@pytest.fixture(params=("ram", "stage0"))
def ums_image(request: pytest.FixtureRequest, tmp_path: Path) -> UmsImage:
    """Create independent payload layouts with a 64-aligned 512-byte zero slot."""
    kernel = tmp_path / "kernel.bin"
    dtb = tmp_path / "target.dtb"
    image = tmp_path / "ramboot.bin"
    map_file = tmp_path / "ramboot.map"
    symbols = {"__image_start": 0x80100000, "FPLINUX_BOOTSTRAP_STORAGE_DISABLED": 1}
    uboot = None
    if request.param == "ram":
        payload = bytearray(40)
        payload[36:40] = b"\x18\x28\x6f\x01"
        tree = binary_tree(
            [],
            [
                ("chosen", [("rng-seed", bytes([0xA1]) * 64)], []),
                (
                    "fplinux-session",
                    [
                        ("compatible", b"fplinux,ram-session\0"),
                        ("fplinux,ssh-client-key", bytes([0xB2]) * 68),
                        ("fplinux,session-id", bytes([0xC3]) * 32),
                        ("fplinux,usb-session", bytes([0xD4]) * 256),
                    ],
                    [],
                ),
            ],
        )
        dtb.write_bytes(tree)
        dtb_offset = 576
        end = dtb_offset + len(tree)
        symbols.update(
            {
                "linux_zimage_start": 0x80100200,
                "linux_zimage_end": 0x80100228,
                "linux_dtb_start": 0x80100000 + dtb_offset,
                "linux_dtb_end": 0x80100000 + end,
            }
        )
    else:
        payload = bytearray(b"independent U-Boot bytes")
        tree = b""
        dtb_offset = 0
        end = 512 + len(payload)
        symbols.update(
            {
                "__bss_end": 0x80108000,
                "uboot_payload_start": 0x80100200,
                "uboot_payload_end": 0x80100000 + end,
                "stage0_ops": 0x80100080,
                "uboot_handoff": 0x801000C0,
            }
        )
        uboot = UbootBuild(kernel, kernel, kernel, kernel, dtb, map_file, kernel, 0x83800000, {})
    kernel.write_bytes(payload)
    slot = (end + 63) & ~63
    contents = bytearray(slot + 512)
    contents[:4] = b"DHTB"
    struct.pack_into("<I", contents, 48, len(contents) - 512)
    contents[512 : 512 + len(payload)] = payload
    if tree:
        contents[dtb_offset : dtb_offset + len(tree)] = tree
    image.write_bytes(contents)
    symbols["fplinux_session_start"] = 0x80100000 + slot
    symbols["fplinux_session_end"] = 0x80100000 + slot + 512
    fixture = UmsImage(image, kernel, dtb, map_file, symbols, uboot, slot)
    fixture.write_symbols()
    return fixture


def test_session_descriptor_preserves_offset_size_and_zero_template(ums_image: UmsImage) -> None:
    """Both build modes publish the same fixed slot ABI without altering the image."""
    before = ums_image.image.read_bytes()

    assert ums_image.verify(payload_limit=0x80100000 + len(before) + 1) == {
        "offset": ums_image.slot,
        "bytes": 512,
        "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
    }
    assert ums_image.image.read_bytes() == before


@pytest.mark.parametrize("contents", [b"DHTB", b"NOPE" + bytes(508)])
def test_image_rejects_an_incomplete_dhtb_header(ums_image: UmsImage, contents: bytes) -> None:
    """The declared-size field is never read from a partial or foreign image."""
    ums_image.image.write_bytes(contents)

    with pytest.raises(SystemExit, match="output does not have a complete DHTB header"):
        ums_image.verify()


def test_image_rejects_a_dhtb_length_beyond_its_bytes(ums_image: UmsImage) -> None:
    """The envelope cannot declare one byte more payload than the file contains."""
    image = bytearray(ums_image.image.read_bytes())
    struct.pack_into("<I", image, 48, len(image) - 511)
    ums_image.image.write_bytes(image)

    with pytest.raises(SystemExit, match="DHTB declared size exceeds the RAM image"):
        ums_image.verify()


def test_image_rejects_touching_the_payload_limit(ums_image: UmsImage) -> None:
    """The reserved upper bound remains exclusive in both build modes."""
    with pytest.raises(SystemExit, match="reaches its declared payload limit"):
        ums_image.verify(payload_limit=0x80100000 + ums_image.image.stat().st_size)


@pytest.mark.parametrize("shift", [1, 64], ids=("unaligned", "aligned-gap"))
def test_session_slot_must_immediately_follow_its_aligned_payload(
    ums_image: UmsImage, shift: int
) -> None:
    """A misplaced slot cannot be used for subsequent session personalization."""
    ums_image.symbols["fplinux_session_start"] += shift
    ums_image.symbols["fplinux_session_end"] += shift
    ums_image.write_symbols()

    with pytest.raises(SystemExit, match="slot does not immediately follow"):
        ums_image.verify()


@pytest.mark.parametrize("slot_bytes", [511, 513])
def test_session_slot_requires_the_exact_abi_size(ums_image: UmsImage, slot_bytes: int) -> None:
    """Neither a short nor an enlarged slot satisfies the published session ABI."""
    ums_image.symbols["fplinux_session_end"] = (
        ums_image.symbols["fplinux_session_start"] + slot_bytes
    )
    ums_image.write_symbols()

    with pytest.raises(SystemExit, match="slot does not have its exact ABI size"):
        ums_image.verify()


def test_session_slot_rejects_a_truncated_zero_template(ums_image: UmsImage) -> None:
    """Even zero-valued bytes cannot supply a slot extending beyond the actual image."""
    image = bytearray(ums_image.image.read_bytes()[:-1])
    struct.pack_into("<I", image, 48, len(image) - 512)
    ums_image.image.write_bytes(image)
    error = (
        "slot lies outside the RAM image" if ums_image.uboot is None else "slot is not all zero"
    )

    with pytest.raises(SystemExit, match=error):
        ums_image.verify()


@pytest.mark.parametrize("offset", [0, 511], ids=("first-byte", "last-byte"))
def test_session_slot_rejects_existing_personalization(ums_image: UmsImage, offset: int) -> None:
    """Built artifacts must never carry a previous session's private bytes."""
    image = bytearray(ums_image.image.read_bytes())
    image[ums_image.slot + offset] = 1
    ums_image.image.write_bytes(image)

    with pytest.raises(SystemExit, match="slot is not all zero"):
        ums_image.verify()
