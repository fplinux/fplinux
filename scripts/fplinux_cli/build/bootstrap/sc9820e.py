# SPDX-License-Identifier: GPL-2.0-only
"""Build a bounded ARM entry and pack separate SC9820E RAM upload regions."""

from __future__ import annotations

import shutil
import struct
from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine.rootfs_state import INITRAMFS_NAME
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    all_path_properties,
    memory_reservations,
)
from fplinux_cli.common import fail, sha256_bytes
from fplinux_cli.templates import substitute_template

from .result import BootstrapBuild
from .verify import (
    RAM_SESSION_ALIGNMENT,
    RAM_SESSION_BYTES,
    _verify_session_dtb,
    verify_forbidden_dtb_markers,
)

if TYPE_CHECKING:
    from pathlib import Path

    from .result import PayloadRegion


PLATFORM_SOURCE = "platforms/sc9820e/bootstrap"


def generated_entry_files(layout: dict[str, int], *, kernel_bytes: int) -> dict[str, bytes]:
    """Substitute target placement and the actual copied Image extent into resources."""
    if not 0 < kernel_bytes <= 0xFFFFFFFF:
        fail("SC9820E kernel copy size must fit in one nonzero ARM register")
    values = {name.upper(): f"0x{value:08x}" for name, value in layout.items()}
    values["KERNEL_BYTES"] = f"0x{kernel_bytes:08x}"
    source = inputs_build.root_source(PLATFORM_SOURCE)
    return {
        name: substitute_template((source / f"{name}.in").read_text(), values).encode("ascii")
        for name in ("entry.s", "entry.ld")
    }


def verify_entry_load(elf: bytes, binary: bytes, layout: dict[str, int]) -> None:
    """Require the complete single executable ELF LOAD, without implicit BSS."""
    if (
        len(elf) < 52
        or elf[:7] != b"\x7fELF\x01\x01\x01"
        or struct.unpack_from("<H", elf, 18)[0] != 40
        or struct.unpack_from("<I", elf, 24)[0] != layout["entry_load"]
    ):
        fail("SC9820E entry ELF identity or entry address is invalid")
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    if phentsize != 32 or phoff + phnum * phentsize > len(elf):
        fail("SC9820E entry ELF program headers are invalid")
    loads = []
    for position in range(phoff, phoff + phnum * phentsize, phentsize):
        header = struct.unpack_from("<8I", elf, position)
        if header[0] == 1:
            loads.append(header)
    if len(loads) != 1:
        fail("SC9820E entry must have exactly one ELF LOAD")
    _kind, offset, virtual, physical, file_size, memory_size, flags, _alignment = loads[0]
    load = layout["entry_load"]
    if (
        virtual != load
        or physical != load
        or flags != 5
        or file_size != memory_size
        or file_size != len(binary)
        or not binary
        or load + file_size > layout["entry_limit"]
        or offset + file_size > len(elf)
        or elf[offset : offset + file_size] != binary
    ):
        fail("SC9820E binary is not the complete bounded executable entry LOAD")


def _kernel_memory_range(system_map: Path, kernel_load: int, image_size: int) -> tuple[int, int]:
    """Translate the actual linked kernel, BSS and page directory into physical RAM."""
    symbols: dict[str, int] = {}
    for line in inputs_build.require_file(system_map).read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in {"_text", "_edata", "_end", "swapper_pg_dir"}:
            if fields[2] in symbols:
                fail(f"kernel System.map repeats {fields[2]}")
            symbols[fields[2]] = int(fields[0], 16)
    if set(symbols) != {"_text", "_edata", "_end", "swapper_pg_dir"}:
        fail("kernel System.map lacks raw Image memory-bound symbols")
    text = symbols["_text"]
    if not symbols["swapper_pg_dir"] < text <= symbols["_edata"] <= symbols["_end"]:
        fail("kernel System.map has invalid raw Image memory bounds")
    if image_size != symbols["_edata"] - text:
        fail("raw Image size differs from its linked initialized extent")
    return (
        kernel_load + symbols["swapper_pg_dir"] - text,
        kernel_load + symbols["_end"] - text,
    )


def _dtb_layout(tree: bytes, memory: dict[str, int]) -> list[tuple[str, int, int]]:
    """Verify declared RAM and collect all current static reservations."""
    try:
        nodes = all_path_properties(tree)
        reservations = memory_reservations(tree)
    except DeviceTreeError as error:
        fail(str(error))
    base, size = memory["base"], memory["size"]
    root = nodes["/"]
    if root.get("#address-cells") != struct.pack(">I", 1) or root.get(
        "#size-cells"
    ) != struct.pack(">I", 1):
        fail("SC9820E DTB must use 32-bit RAM address and size cells")
    ram_nodes = [node for node in nodes.values() if node.get("device_type") == b"memory\0"]
    if len(ram_nodes) != 1 or ram_nodes[0].get("reg") != struct.pack(">II", base, size):
        fail("SC9820E DTB RAM differs from the target Linux memory range")
    result = [("FDT reservation", address, address + length) for address, length in reservations]
    reserved = nodes.get("/reserved-memory", {})
    if reserved and (
        reserved.get("#address-cells") != struct.pack(">I", 1)
        or reserved.get("#size-cells") != struct.pack(">I", 1)
        or reserved.get("ranges") != b""
    ):
        fail("SC9820E DTB reserved-memory has unsupported address translation")
    for path, node in nodes.items():
        if not path.startswith("/reserved-memory/") or "/" in path.removeprefix(
            "/reserved-memory/"
        ):
            continue
        if node.get("status", b"okay\0") == b"disabled\0":
            continue
        value = node.get("reg", b"")
        if not value or len(value) % 8:
            fail(f"SC9820E DTB reservation {path} has no static address/size pairs")
        for address, length in struct.iter_unpack(">II", value):
            result.append((path, address, address + length))
    for name, start, end in result:
        if not base <= start <= end <= base + size:
            fail(f"SC9820E DTB {name} lies outside Linux RAM")
    return result


def verify_ram_layout(  # noqa: PLR0913 -- actual artifact extents have distinct roles.
    *,
    layout: dict[str, int],
    memory: dict[str, int],
    tree: bytes,
    entry_size: int,
    kernel_size: int,
    initramfs_size: int | None,
    system_map: Path,
    loader_stack_guard: tuple[int, int],
) -> None:
    """Reject upload/runtime overlap with linked Linux and preserved reservations."""
    kernel_start, kernel_end = _kernel_memory_range(system_map, layout["kernel_load"], kernel_size)
    runtime_regions = [
        ("entry", layout["entry_load"], layout["entry_load"] + entry_size),
        ("kernel and page tables", kernel_start, kernel_end),
        ("DTB", layout["fdt_load"], layout["fdt_load"] + len(tree)),
    ]
    if initramfs_size is not None:
        runtime_regions.append(
            ("initramfs", layout["initramfs_load"], layout["initramfs_load"] + initramfs_size)
        )
    runtime_regions.append(("handoff stack", layout["stack"] - 24, layout["stack"]))
    staging = ("kernel staging", layout["kernel_stage"], layout["kernel_stage"] + kernel_size)
    if staging[2] > layout["kernel_stage_limit"]:
        fail("SC9820E kernel staging exceeds its declared load limit")
    regions = [*runtime_regions, staging]
    base, end = memory["base"], memory["base"] + memory["size"]
    if layout["stack"] % 8 or layout["kernel_load"] % 0x8000 or layout["fdt_load"] % 8:
        fail("SC9820E kernel, DTB or handoff stack alignment is invalid")
    if not layout["entry_load"] < layout["entry_limit"] <= kernel_start:
        fail("SC9820E entry limit reaches the Linux page directory")
    for name, start, limit in regions:
        if not base <= start < limit <= end:
            fail(f"SC9820E {name} lies outside Linux RAM")
    for index, (name, start, limit) in enumerate(regions):
        for other_name, other_start, other_limit in regions[index + 1 :]:
            if start < other_limit and other_start < limit:
                fail(f"SC9820E {name} overlaps {other_name}")
    uploads = [runtime_regions[0], staging, runtime_regions[2]]
    if initramfs_size is not None:
        uploads.append(runtime_regions[3])
    guard_start, guard_end = loader_stack_guard
    if not base <= guard_start < guard_end <= end:
        fail("SC9820E loader stack guard lies outside Linux RAM")
    for name, start, limit in uploads:
        if start < guard_end and guard_start < limit:
            fail(f"SC9820E uploaded {name} overlaps the loader stack guard")
    reservations = _dtb_layout(tree, memory)
    for name, start, limit in [*runtime_regions[:-1], staging]:
        for reserved_name, reserved_start, reserved_end in reservations:
            if reserved_start == reserved_end or start >= reserved_end or reserved_start >= limit:
                continue
            # The resident entry is deliberately excluded from Linux allocation.
            if name == "entry" and reserved_start <= start < limit <= reserved_end:
                continue
            fail(f"SC9820E {name} overlaps {reserved_name}")


def pack_payload(
    parts: dict[str, tuple[bytes, int]], image: Path
) -> tuple[dict[str, PayloadRegion], dict[str, int | str]]:
    """Pack the selected RAM regions followed by the common canonical session slot."""
    if tuple(parts) not in (
        ("entry", "kernel", "dtb"),
        ("entry", "kernel", "dtb", "initramfs"),
    ):
        fail("SC9820E payload requires entry, kernel, DTB and optional initramfs in order")
    contents = bytearray()
    regions: dict[str, PayloadRegion] = {}
    for name, (data, address) in parts.items():
        if not data:
            fail(f"SC9820E {name} is empty")
        contents.extend(bytes(-len(contents) % RAM_SESSION_ALIGNMENT))
        regions[name] = {"offset": len(contents), "size": len(data), "load_address": address}
        contents.extend(data)
    contents.extend(bytes(-len(contents) % RAM_SESSION_ALIGNMENT))
    offset = len(contents)
    template = bytes(RAM_SESSION_BYTES)
    contents.extend(template)
    image.parent.mkdir(parents=True, exist_ok=True)
    image.write_bytes(contents)
    return regions, {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }


def _patch_initrd(dtb: Path, initramfs: Path, address: int) -> bytes:
    """Set the two existing chosen cells while preserving every other DT value."""
    original = dtb.read_bytes()
    try:
        before = all_path_properties(original)
        reservations = memory_reservations(original)
    except DeviceTreeError as error:
        fail(str(error))
    chosen = before.get("/chosen", {})
    for name, marker in (
        ("linux,initrd-start", bytes([0xE5]) * 4),
        ("linux,initrd-end", bytes([0xF6]) * 4),
    ):
        if chosen.get(name) != marker:
            fail(f"SC9820E DTB /chosen {name} must contain its canonical marker")
    end = address + initramfs.stat().st_size
    if not 0 <= address < end <= 0xFFFFFFFF:
        fail("SC9820E initramfs extent does not fit its 32-bit DT cells")
    for name, value in (("linux,initrd-start", address), ("linux,initrd-end", end)):
        process_build.run(["fdtput", "-t", "x", str(dtb), "/chosen", name, f"{value:x}"])
        chosen[name] = struct.pack(">I", value)
    tree = dtb.read_bytes()
    if all_path_properties(tree) != before or memory_reservations(tree) != reservations:
        fail("SC9820E initramfs DT update changed unrelated properties or reservations")
    _verify_session_dtb(tree)
    return tree


def build_sc9820e(  # noqa: PLR0913 -- source, placement and output inputs stay explicit.
    target: str,
    target_config: dict[str, Any],
    *,
    work: Path,
    kernel: Path,
    dtb: Path,
    kernel_output: Path,
    rootfs_output: Path,
) -> BootstrapBuild:
    """Compile the complete source entry, verify RAM placement and pack public bytes."""
    bootstrap = target_config["bootstrap"]
    layout = bootstrap["layout"]
    kernel_bytes = inputs_build.require_file(kernel).read_bytes()
    work.mkdir(parents=True)
    for name, contents in generated_entry_files(layout, kernel_bytes=len(kernel_bytes)).items():
        (work / name).write_bytes(contents)
    platform_source = inputs_build.root_source(PLATFORM_SOURCE)
    target_source = inputs_build.target_source(target, bootstrap["source"])
    prefix = bootstrap["toolchain"] + "-"
    objects = []
    for source in (
        work / "entry.s",
        target_source / "src/linux-entry.s",
        platform_source / "src/linux-handoff.s",
        platform_source / "src/kernel-copy.c",
    ):
        output = work / f"{source.stem}.o"
        c_flags = (
            [
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-builtin",
                "-fno-stack-protector",
                "-fno-tree-loop-distribute-patterns",
                "-fno-unwind-tables",
                "-fno-asynchronous-unwind-tables",
                "-Wa,--noexecstack",
            ]
            if source.suffix == ".c"
            else []
        )
        process_build.run(
            [
                prefix + "gcc",
                "-c",
                "-mcpu=cortex-a53",
                "-marm",
                "-mfloat-abi=soft",
                "-ffreestanding",
                "-Os",
                *c_flags,
                "-o",
                str(output),
                str(inputs_build.require_file(source)),
            ]
        )
        objects.append(str(output))
    elf, binary = work / "entry.elf", work / "entry.bin"
    map_file = work / bootstrap["map"]
    map_file.parent.mkdir(parents=True, exist_ok=True)
    process_build.run(
        [
            prefix + "ld",
            "--fatal-warnings",
            "--orphan-handling=error",
            "--no-undefined",
            "-T",
            str(work / "entry.ld"),
            "-Map",
            str(map_file),
            "-o",
            str(elf),
            *objects,
        ]
    )
    process_build.run([prefix + "objcopy", "-O", "binary", str(elf), str(binary)])
    entry = inputs_build.require_file(binary).read_bytes()
    verify_entry_load(inputs_build.require_file(elf).read_bytes(), entry, layout)
    staged_dtb = work / "linux.dtb"
    shutil.copyfile(inputs_build.require_file(dtb), staged_dtb)
    initramfs = None
    if bootstrap["initramfs_delivery"] == "external":
        initramfs = inputs_build.require_file(rootfs_output / INITRAMFS_NAME)
        tree = _patch_initrd(staged_dtb, initramfs, layout["initramfs_load"])
    elif bootstrap["initramfs_delivery"] == "none":
        tree = staged_dtb.read_bytes()
        _verify_session_dtb(tree)
        chosen = all_path_properties(tree)["/chosen"]
        if {"linux,initrd-start", "linux,initrd-end"} & chosen.keys():
            fail("SC9820E external root DTB must not contain initramfs properties")
    else:
        fail("SC9820E bootstrap requires external or absent initramfs delivery")
    verify_forbidden_dtb_markers(tree, target_config["linux"]["forbidden_dtb_markers"])
    verify_ram_layout(
        layout=layout,
        memory=target_config["linux"]["memory"],
        tree=tree,
        entry_size=len(entry),
        kernel_size=len(kernel_bytes),
        initramfs_size=initramfs.stat().st_size if initramfs is not None else None,
        system_map=kernel_output / "System.map",
        loader_stack_guard=(
            target_config["loader"]["stack_guard_start"],
            target_config["loader"]["stack_guard_end"],
        ),
    )
    image = work / bootstrap["image"]
    parts = {
        "entry": (entry, layout["entry_load"]),
        "kernel": (kernel_bytes, layout["kernel_stage"]),
        "dtb": (tree, layout["fdt_load"]),
    }
    if initramfs is not None:
        parts["initramfs"] = (initramfs.read_bytes(), layout["initramfs_load"])
    regions, personalization = pack_payload(parts, image)
    return BootstrapBuild(image, map_file, personalization, regions, staged_dtb)
