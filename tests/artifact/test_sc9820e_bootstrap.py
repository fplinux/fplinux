# SPDX-License-Identifier: GPL-2.0-only
"""Build the source entry and compact RAM artifact through the real bootstrap stage."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.bootstrap.stage import build_bootstrap
from fplinux_cli.build.device_tree import exact_path_properties, memory_reservations
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from tests.fdt import binary_tree
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.bootstrap.result import BootstrapBuild


@pytest.mark.parametrize("forbidden_storage", [False, True], ids=["allowed", "forbidden-storage"])
@pytest.mark.parametrize("external_root", [False, True], ids=["ram-root", "ext4-root"])
def test_source_entry_and_dtb_preserve_the_selected_root_artifact_contract(
    tmp_path: Path,
    *,
    forbidden_storage: bool,
    external_root: bool,
) -> None:
    """Real ARM tools retain complete entry bytes; fdtput changes only the staged DTB."""
    target = load_target("xiaomi-qin-1s-plus", "microsd-root" if external_root else None)
    platform = load_platform("sc9820e")
    kernel_output = tmp_path / "kernel"
    kernel_output.mkdir()
    kernel = kernel_output / "Image"
    kernel.write_bytes(bytes(range(256)))
    (kernel_output / "System.map").write_text(
        "c0004000 B swapper_pg_dir\nc0008000 T _text\nc0008100 D _edata\nc000a000 B _end\n"
    )
    rootfs_output = tmp_path / "rootfs"
    rootfs_output.mkdir()
    initramfs = rootfs_output / "initramfs.cpio"
    if not external_root:
        initramfs.write_bytes(b"independent initramfs bytes\n")
    root_properties = (
        [("bootargs", b"root=PARTUUID=46504c52-02 rootfstype=ext4 rw init=/sbin/init\0")]
        if external_root
        else [
            ("linux,initrd-start", bytes([0xE5]) * 4),
            ("linux,initrd-end", bytes([0xF6]) * 4),
            ("bootargs", b"rdinit=/init\0"),
        ]
    )
    tree = binary_tree(
        [("#address-cells", b"\0\0\0\1"), ("#size-cells", b"\0\0\0\1")],
        [
            (
                "chosen",
                [
                    ("rng-seed", bytes([0xA1]) * 64),
                    *root_properties,
                ],
                [],
            ),
            (
                "memory@80000000",
                [("device_type", b"memory\0"), ("reg", bytes.fromhex("8000000020000000"))],
                [],
            ),
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
            *(
                [("storage", [("compatible", b"example,spi-nand\0")], [])]
                if forbidden_storage
                else []
            ),
        ],
        reservations=((0x80002000, 0x2000), (0x9EFF0000, 0x01010000)),
    )
    dtb = kernel_output / "target.dtb"
    dtb.write_bytes(tree)

    def build_payload() -> BootstrapBuild:
        """Compile and pack the isolated files through the public stage operation."""
        return build_bootstrap(
            {},
            "xiaomi-qin-1s-plus",
            target,
            platform,
            work=tmp_path / "work",
            zimage=kernel,
            dtb=dtb,
            kernel_output=kernel_output,
            rootfs_output=rootfs_output,
        )

    if forbidden_storage:
        with pytest.raises(SystemExit, match="forbidden storage marker spi-nand"):
            build_payload()
        assert dtb.read_bytes() == tree
        return
    result = build_payload()

    assert dtb.read_bytes() == tree
    assert result.dtb is not None
    chosen = exact_path_properties(result.dtb, "/chosen")["/chosen"]
    if external_root:
        assert "linux,initrd-start" not in chosen
        assert "linux,initrd-end" not in chosen
        assert chosen["bootargs"] == (
            b"root=PARTUUID=46504c52-02 rootfstype=ext4 rw init=/sbin/init\0"
        )
        assert not initramfs.exists()
    else:
        assert chosen["linux,initrd-start"] == bytes.fromhex("85500000")
        assert chosen["linux,initrd-end"] == bytes.fromhex("8550001c")
        assert chosen["bootargs"] == b"rdinit=/init\0"
    assert memory_reservations(result.dtb) == ((0x80002000, 0x2000), (0x9EFF0000, 0x01010000))
    regions = result.regions
    assert regions is not None
    assert set(regions) == (
        {"entry", "kernel", "dtb"} if external_root else {"entry", "kernel", "dtb", "initramfs"}
    )
    payload = result.image.read_bytes()
    sources = [
        ("entry", result.image.parent / "entry.bin"),
        ("kernel", kernel),
        ("dtb", result.dtb),
    ]
    if not external_root:
        sources.append(("initramfs", initramfs))
    for name, source in sources:
        region = regions[name]
        assert region["offset"] % 64 == 0
        assert payload[region["offset"] : region["offset"] + region["size"]] == source.read_bytes()
    assert regions["entry"]["load_address"] == 0x80002060
    assert regions["kernel"]["load_address"] == 0x81008000
    assert regions["dtb"]["load_address"] == 0x85400000
    if not external_root:
        assert regions["initramfs"]["load_address"] == 0x85500000
    assert payload[-512:] == bytes(512)
    source_bytes = sum(source.stat().st_size for _name, source in sources)
    assert len(payload) <= source_bytes + 252 + 512
    # Linker map and ELF identify the executable source entry, including its helpers.
    assert result.map.stat().st_size > 0
    elf = (result.image.parent / "entry.elf").read_bytes()
    assert struct.unpack_from("<I", elf, 24)[0] == 0x80002060
    undefined = run_process(
        ["arm-none-eabi-nm", "-u", str(result.image.parent / "entry.elf")],
        name="check the complete ARM entry has no unresolved runtime helpers",
        timeout=10,
        check=True,
    )
    assert undefined.stdout == ""
