# SPDX-License-Identifier: GPL-2.0-only
"""Artifact bytes, memory placement and causal inputs of compact SC9820E payloads."""

from __future__ import annotations

import hashlib
import shutil
import struct
from contextlib import nullcontext
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build.bootstrap.recipe import bootstrap_recipe_digest
from fplinux_cli.build.bootstrap.sc9820e import (
    pack_payload,
    verify_entry_load,
    verify_ram_layout,
)
from fplinux_cli.build.device_tree import all_path_properties, memory_reservations

from tests import ROOT
from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def ram_layout() -> dict[str, int]:
    """Supply target-owned physical addresses without production layout helpers."""
    return {
        "entry_load": 0x80002060,
        "entry_limit": 0x80004000,
        "kernel_load": 0x80008000,
        "kernel_stage": 0x81008000,
        "kernel_stage_limit": 0x82000000,
        "fdt_load": 0x85400000,
        "initramfs_load": 0x85500000,
        "stack": 0x9FFEFF50,
    }


@pytest.fixture
def system_map(tmp_path: Path) -> Path:
    """Represent a linked kernel with a page directory, initialized bytes and BSS."""
    path = tmp_path / "System.map"
    path.write_text(
        "c0004000 B swapper_pg_dir\nc0008000 T _text\nc0008100 D _edata\nc000a000 B _end\n"
    )
    return path


@pytest.fixture
def ram_tree() -> bytes:
    """Build independent RAM and reservation values in the binary FDT format."""
    return binary_tree(
        [("#address-cells", b"\0\0\0\1"), ("#size-cells", b"\0\0\0\1")],
        [
            (
                "memory@80000000",
                [("device_type", b"memory\0"), ("reg", bytes.fromhex("8000000020000000"))],
                [],
            ),
            (
                "reserved-memory",
                [
                    ("#address-cells", b"\0\0\0\1"),
                    ("#size-cells", b"\0\0\0\1"),
                    ("ranges", b""),
                ],
                [("radio@88300000", [("reg", bytes.fromhex("883000000015a800"))], [])],
            ),
        ],
        reservations=((0x80002000, 0x2000), (0x9EFF0000, 0x01010000)),
    )


def test_whole_tree_and_reservations_preserve_exact_paths(ram_tree: bytes) -> None:
    """Whole-tree consumers see nested reservations and the original FDT reserve map."""
    nodes = all_path_properties(ram_tree)
    assert nodes["/reserved-memory/radio@88300000"] == {"reg": bytes.fromhex("883000000015a800")}
    assert nodes["/memory@80000000"]["reg"] == bytes.fromhex("8000000020000000")
    assert memory_reservations(ram_tree) == ((0x80002000, 0x2000), (0x9EFF0000, 0x01010000))


def test_payload_preserves_four_exact_regions_without_ram_address_padding(tmp_path: Path) -> None:
    """Each upload uses exact source bytes; large RAM gaps cost only alignment bytes."""
    image = tmp_path / "ramboot.bin"
    regions, session = pack_payload(
        {
            "entry": (b"entry\0helper", 0x80002060),
            "kernel": (b"kernel" * 12, 0x81008000),
            "dtb": (b"tree", 0x85400000),
            "initramfs": (b"ram", 0x85500000),
        },
        image,
    )
    assert regions == {
        "entry": {"offset": 0, "size": 12, "load_address": 0x80002060},
        "kernel": {"offset": 64, "size": 72, "load_address": 0x81008000},
        "dtb": {"offset": 192, "size": 4, "load_address": 0x85400000},
        "initramfs": {"offset": 256, "size": 3, "load_address": 0x85500000},
    }
    assert session == {
        "offset": 320,
        "bytes": 512,
        "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
    }
    contents = image.read_bytes()
    assert len(contents) == 832
    assert contents[:12] == b"entry\0helper"
    assert contents[64:136] == b"kernel" * 12
    assert contents[192:196] == b"tree"
    assert contents[256:259] == b"ram"
    assert contents[320:] == bytes(512)


def test_entry_binary_must_include_the_complete_executable_load(
    ram_layout: dict[str, int],
) -> None:
    """An exported function prefix cannot silently drop its trailing helper/literal bytes."""
    body = b"entry\0local-helper\0literal-pool\0"
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + bytes(9),
        2,
        40,
        1,
        0x80002060,
        52,
        0,
        0x05000000,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    program = struct.pack("<8I", 1, 128, 0x80002060, 0x80002060, len(body), len(body), 5, 4)
    elf = header + program + bytes(44) + body
    verify_entry_load(elf, body, ram_layout)
    with pytest.raises(SystemExit, match="complete bounded executable entry LOAD"):
        verify_entry_load(elf, body[:6], ram_layout)
    with pytest.raises(SystemExit, match="complete bounded executable entry LOAD"):
        verify_entry_load(elf, body, {**ram_layout, "entry_limit": 0x80002070})


def test_external_root_payload_retains_session_without_an_initramfs(tmp_path: Path) -> None:
    """The external-root loader receives only three exact uploads and its session slot."""
    image = tmp_path / "external-root.bin"
    regions, session = pack_payload(
        {
            "entry": (b"entry", 0x80002060),
            "kernel": (b"kernel", 0x81008000),
            "dtb": (b"tree", 0x85400000),
        },
        image,
    )
    assert regions == {
        "entry": {"offset": 0, "size": 5, "load_address": 0x80002060},
        "kernel": {"offset": 64, "size": 6, "load_address": 0x81008000},
        "dtb": {"offset": 128, "size": 4, "load_address": 0x85400000},
    }
    assert session["offset"] == 192
    assert session["bytes"] == 512
    assert image.read_bytes()[192:] == bytes(512)


def test_external_root_ram_placement_checks_payload_without_claiming_an_initrd_arena(
    ram_layout: dict[str, int], ram_tree: bytes, system_map: Path
) -> None:
    """The absent archive has no RAM extent; real DTB and kernel bounds still apply."""
    layout = {**ram_layout, "initramfs_load": 0x88300100}
    verify_ram_layout(
        layout=layout,
        memory={"base": 0x80000000, "size": 0x20000000},
        tree=ram_tree,
        entry_size=760,
        kernel_size=256,
        initramfs_size=None,
        system_map=system_map,
        loader_stack_guard=(0x808E0000, 0x808F0000),
    )
    with pytest.raises(SystemExit, match="DTB overlaps FDT reservation"):
        verify_ram_layout(
            layout={**layout, "fdt_load": 0x9EFF0000},
            memory={"base": 0x80000000, "size": 0x20000000},
            tree=ram_tree,
            entry_size=760,
            kernel_size=256,
            initramfs_size=None,
            system_map=system_map,
            loader_stack_guard=(0x808E0000, 0x808F0000),
        )


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({}, None),
        ({"entry_limit": 0x80004004}, "page directory"),
        ({"kernel_stage": 0x80009000}, "kernel and page tables overlaps kernel staging"),
        ({"kernel_stage": 0x808E0000}, "uploaded kernel staging overlaps the loader stack guard"),
        ({"kernel_stage": 0x81FFFF80}, "kernel staging exceeds its declared load limit"),
        ({"fdt_load": 0x80009000}, "kernel and page tables overlaps DTB"),
        ({"initramfs_load": 0x88300100}, "initramfs overlaps /reserved-memory/radio"),
        ({"fdt_load": 0x9EFF0000}, "DTB overlaps FDT reservation"),
        ({"initramfs_load": 0x9FFFFFF0}, "initramfs lies outside Linux RAM"),
    ],
)
def test_ram_placement_preserves_linked_bss_page_tables_and_reservations(
    ram_layout: dict[str, int],
    ram_tree: bytes,
    system_map: Path,
    changes: dict[str, int],
    message: str | None,
) -> None:
    """Real initialized/BSS and reservation extents decide whether upload is safe."""
    expected_error = (
        pytest.raises(SystemExit, match=message) if message is not None else nullcontext()
    )
    with expected_error:
        verify_ram_layout(
            layout={**ram_layout, **changes},
            memory={"base": 0x80000000, "size": 0x20000000},
            tree=ram_tree,
            entry_size=760,
            kernel_size=256,
            initramfs_size=4096,
            system_map=system_map,
            loader_stack_guard=(0x808E0000, 0x808F0000),
        )


def test_runtime_kernel_may_reclaim_the_loader_stack_after_staged_upload(
    ram_layout: dict[str, int], ram_tree: bytes, system_map: Path
) -> None:
    """A current-sized raw Image crosses the retired stack while its upload stays separate."""
    system_map.write_text(
        "c0004000 A swapper_pg_dir\nc0008000 T _text\nc095077c D _edata\nc0970614 B _end\n"
    )
    verify_ram_layout(
        layout=ram_layout,
        memory={"base": 0x80000000, "size": 0x20000000},
        tree=ram_tree,
        entry_size=1024,
        kernel_size=9734012,
        initramfs_size=4096,
        system_map=system_map,
        loader_stack_guard=(0x808E0000, 0x808F0000),
    )


def test_sc_recipe_tracks_entry_layout_and_loader_guard_without_firmware_or_gui_inputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, ram_layout: dict[str, int]
) -> None:
    """Entry, staging and guard changes miss while run-side firmware and GUI remain hits."""
    implementation = inputs_build.bootstrap_implementation_sources("sc9820e")
    for relative, _source in implementation:
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(b"implementation\n")
    shutil.copytree(ROOT / "platforms/sc9820e/bootstrap", tmp_path / "platforms/sc9820e/bootstrap")
    target_source = tmp_path / "targets/demo/bootstrap"
    target_source.mkdir(parents=True)
    entry = target_source / "entry.s"
    entry.write_bytes(b"entry implementation\n")
    target: dict[str, Any] = {
        "bootstrap": {"source": "bootstrap", "layout": ram_layout, "toolchain": "arm-none-eabi"},
        "linux": {"memory": {"base": 0x80000000, "size": 0x20000000}},
        "loader": {"stack_guard_start": 0x808E0000, "stack_guard_end": 0x808F0000},
    }
    platform = {"bootstrap": {"backend": "sc9820e", "toolchain": "arm-none-eabi"}}
    monkeypatch.setattr(common, "ROOT", tmp_path)
    baseline = bootstrap_recipe_digest({}, "demo", target, platform)
    target["loader"]["firmware"] = "run-only input"
    target["display"] = {"width": 320, "height": 240}
    assert bootstrap_recipe_digest({}, "demo", target, platform) == baseline
    target["loader"]["stack_guard_end"] -= 4
    assert bootstrap_recipe_digest({}, "demo", target, platform) != baseline
    target["loader"]["stack_guard_end"] += 4
    entry.write_bytes(b"different entry implementation\n")
    assert bootstrap_recipe_digest({}, "demo", target, platform) != baseline
    entry.write_bytes(b"entry implementation\n")
    for relative in ("src/kernel-copy.c", "entry.s.in"):
        source = tmp_path / "platforms/sc9820e/bootstrap" / relative
        original = source.read_bytes()
        source.write_bytes(original + b"\n")
        assert bootstrap_recipe_digest({}, "demo", target, platform) != baseline
        source.write_bytes(original)
    ram_layout["kernel_stage"] += 4
    assert bootstrap_recipe_digest({}, "demo", target, platform) != baseline
    ram_layout["kernel_stage"] -= 4
    ram_layout["stack"] -= 8
    assert bootstrap_recipe_digest({}, "demo", target, platform) != baseline
