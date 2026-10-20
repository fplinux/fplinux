# SPDX-License-Identifier: GPL-2.0-only
"""Bundled RAM stages and bounded per-session FDT upload preparation."""

from __future__ import annotations

import os
import shutil
import struct
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.build.device_tree import all_path_properties

from tests import ROOT
from tests.host_process.runtime.sc9820e.fixtures import (
    load_program,
    loader_images,
    prepare_runtime,
    session_record,
    session_tree,
)
from tests.small.runtime.adapter_fixtures import FakeClock

ADAPTER = load_program("sc9820e_host_adapter", ROOT / "platforms/sc9820e/host/adapter.py")


@pytest.fixture
def bootrom_device(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Provide real sysfs-shaped identity files and a harmless character device."""
    selected = tmp_path / "sysfs/3-2.6"
    selected.mkdir(parents=True)
    for filename, value in {
        "idVendor": "1782",
        "idProduct": "4d00",
        "busnum": "3",
        "devnum": "41",
    }.items():
        (selected / filename).write_text(value + "\n", encoding="ascii")
    node = tmp_path / "nodes/003/041"
    node.parent.mkdir(parents=True)
    node.symlink_to("/dev/null")
    monkeypatch.setattr(ADAPTER, "USB_DEVICES", tmp_path / "sysfs")
    monkeypatch.setattr(ADAPTER, "USB_NODES", tmp_path / "nodes")
    return selected


def test_bootrom_selection_waits_for_node_access_after_enumeration(
    bootrom_device: Path,
) -> None:
    clock = FakeClock()
    open_node = os.open

    def access_after_udev(path: Path, flags: int) -> int:
        if clock.now < 0.15:
            raise PermissionError
        return open_node(path, flags)

    with (
        mock.patch.object(ADAPTER.os, "open", side_effect=access_after_udev),
        mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
        mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
    ):
        result = ADAPTER._select_bootrom(  # noqa: SLF001 -- exercise USB selection before upload.
            {"vendor_id": 0x1782, "product_id": 0x4D00}, deadline=300
        )

    assert result == (bootrom_device, 3, 41)
    assert clock.now == pytest.approx(0.15)


@pytest.mark.parametrize(("deadline", "elapsed"), [(300.0, 1.0), (0.12, 0.12)])
@pytest.mark.usefixtures("bootrom_device")
def test_bootrom_selection_bounds_persistent_access_denial(
    deadline: float, elapsed: float
) -> None:
    clock = FakeClock()
    with (
        mock.patch.object(ADAPTER.os, "open", side_effect=PermissionError),
        mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
        mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
        pytest.raises(SystemExit, match="not readable and writable; install the documented udev"),
    ):
        ADAPTER._select_bootrom(  # noqa: SLF001 -- exercise USB selection before upload.
            {"vendor_id": 0x1782, "product_id": 0x4D00}, deadline=deadline
        )

    assert clock.now == pytest.approx(elapsed)


def test_bootrom_selection_rejects_ambiguity_while_waiting_for_node_access(
    bootrom_device: Path,
) -> None:
    clock = FakeClock()

    def second_phone_appears(seconds: float) -> None:
        clock.sleep(seconds)
        shutil.copytree(bootrom_device, bootrom_device.with_name("3-2.7"))

    with (
        mock.patch.object(ADAPTER.os, "open", side_effect=PermissionError),
        mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
        mock.patch.object(ADAPTER.time, "sleep", second_phone_appears),
        pytest.raises(SystemExit, match="more than one BootROM USB device"),
    ):
        ADAPTER._select_bootrom(  # noqa: SLF001 -- exercise USB selection before upload.
            {"vendor_id": 0x1782, "product_id": 0x4D00}, deadline=300
        )


def test_dtb_personalization_changes_only_exact_properties_and_preserves_decoys() -> None:
    tree, record = session_tree(), session_record()
    expected = all_path_properties(tree)
    expected["/chosen"]["rng-seed"] = bytes(range(64))
    expected["/fplinux-session"]["fplinux,session-id"] = bytes(range(1, 33))
    expected["/fplinux-session"]["fplinux,ssh-client-key"] = record[112:180]
    expected["/fplinux-session"]["fplinux,usb-session"] = record[180:436]
    result = ADAPTER.personalize_dtb(tree, record, {"load_address": 0x85500000, "size": 7})
    assert all_path_properties(result) == expected
    assert len(result) == len(tree)


@pytest.mark.parametrize(
    "tree",
    [session_tree(seed_path="wrong-chosen"), session_tree(duplicate=True)],
    ids=["wrong-node", "duplicate-property"],
)
def test_dtb_personalization_rejects_misplaced_or_duplicate_session_properties(
    tree: bytes,
) -> None:
    with pytest.raises(SystemExit, match="DTB"):
        ADAPTER.personalize_dtb(tree, session_record(), {"load_address": 0x85500000, "size": 7})


def test_dtb_personalization_rejects_a_property_escaping_the_structure_block() -> None:
    tree = bytearray(session_tree())
    # The first property header follows the root begin token and padded empty name.
    structure = struct.unpack_from(">I", tree, 8)[0]
    struct.pack_into(">I", tree, structure + 12, 0xFFFFFFFF)
    with pytest.raises(SystemExit, match="property is outside"):
        ADAPTER.personalize_dtb(
            bytes(tree), session_record(), {"load_address": 0x85500000, "size": 7}
        )


def test_prepare_uploads_extracts_private_regions_without_changing_the_session_image(
    tmp_path: Path,
) -> None:
    runtime, session, assets = prepare_runtime(tmp_path)
    before = Path(session["image"]).read_bytes()
    uploads_directory = tmp_path / "uploads"
    uploads_directory.mkdir()
    ADAPTER.adapter_config(runtime["adapter"])
    uploads = ADAPTER.prepare_uploads(tmp_path, runtime, session, uploads_directory)
    assert uploads["entry"].read_bytes() == b"entry123"
    assert uploads["kernel"].read_bytes() == b"kernel123"
    assert uploads["initramfs"].read_bytes() == b"root123"
    assert all_path_properties(uploads["dtb"].read_bytes())["/chosen"]["rng-seed"] == bytes(
        range(64)
    )
    assert all(path.stat().st_mode & 0o777 == 0o600 for path in uploads.values())
    assert Path(session["image"]).read_bytes() == before
    assert {path.name: path.read_bytes() for path in assets.iterdir()} == {
        f"{role}.bin": data for role, data in loader_images().items()
    }
    assert uploads["fdl1"].read_bytes() == b"DHTBfirst-stage"
    assert uploads["fdl2"].read_bytes() == b"DHTBboard-initializer"
    assert uploads["sml"].read_bytes() == b"secure-monitor"


def test_external_root_uploads_personalize_the_same_session_without_an_initramfs(
    tmp_path: Path,
) -> None:
    """Session credentials stay in the RAM DTB while the loader omits the root archive."""
    runtime, session, _assets = prepare_runtime(tmp_path, external_root=True)
    before = Path(session["image"]).read_bytes()
    output = tmp_path / "uploads"
    output.mkdir()
    ADAPTER.adapter_config(runtime["adapter"])
    uploads = ADAPTER.prepare_uploads(tmp_path, runtime, session, output)
    nodes = all_path_properties(uploads["dtb"].read_bytes())
    assert nodes["/chosen"]["bootargs"] == (
        b"root=PARTUUID=46504c52-02 rootfstype=ext4 rw init=/sbin/init\0"
    )
    assert nodes["/chosen"]["rng-seed"] == bytes(range(64))
    assert "linux,initrd-start" not in nodes["/chosen"]
    assert "linux,initrd-end" not in nodes["/chosen"]
    assert nodes["/fplinux-session"]["fplinux,session-id"] == bytes(range(1, 33))
    assert nodes["/fplinux-session"]["fplinux,ssh-client-key"] == session_record()[112:180]
    assert nodes["/fplinux-session"]["fplinux,usb-session"] == session_record()[180:436]
    assert set(uploads) == {"fdl1", "fdl2", "sml", "entry", "kernel", "dtb"}
    assert all(path.stat().st_mode & 0o777 == 0o600 for path in uploads.values())
    assert Path(session["image"]).read_bytes() == before
    arguments = ADAPTER.loader_arguments(
        tmp_path / "loader", uploads, runtime["adapter"], bus=3, device=41
    )
    writes = [
        arguments[index + 4] for index, value in enumerate(arguments) if value == "write_data"
    ]
    assert writes == [str(uploads[role]) for role in ("sml", "kernel", "dtb", "entry")]


@pytest.mark.parametrize("external_root", [False, True], ids=["initramfs-root", "external-root"])
def test_dtb_initrd_cells_must_match_the_selected_root_handoff(*, external_root: bool) -> None:
    """Dropping the upload cannot retain stale initrd cells or weaken session binding."""
    if external_root:
        tree = session_tree()
        initrd = None
    else:
        tree = session_tree(external_root=True)
        initrd = {"load_address": 0x85500000, "size": 7}
    with pytest.raises(SystemExit, match="initramfs"):
        ADAPTER.personalize_dtb(tree, session_record(), initrd)


@pytest.mark.parametrize(
    "damage", ["overlap", "outside", "session-crc", "wrong-session", "initrd-bound"]
)
def test_prepare_uploads_rejects_inconsistent_images_before_an_upload(
    tmp_path: Path, damage: str
) -> None:
    runtime, session, _assets = prepare_runtime(tmp_path)
    regions = runtime["adapter"]["regions"]
    if damage == "overlap":
        regions["kernel"]["offset"] = 0
    elif damage == "outside":
        regions["initramfs"]["size"] = 0xFFFF
    elif damage == "wrong-session":
        session["session_id"] = "0" * 64
    elif damage == "initrd-bound":
        regions["initramfs"]["size"] = 8
    else:
        image = bytearray(Path(session["image"]).read_bytes())
        image[runtime["personalization"]["offset"] + 48] ^= 1
        Path(session["image"]).write_bytes(image)
    output = tmp_path / "uploads"
    output.mkdir()
    with pytest.raises(SystemExit, match="SC9820E"):
        ADAPTER.prepare_uploads(tmp_path, runtime, session, output)


def test_prepare_uploads_rejects_a_missing_bundled_loader(tmp_path: Path) -> None:
    runtime, session, assets = prepare_runtime(tmp_path)
    (assets / "fdl1.bin").unlink()
    output = tmp_path / "uploads"
    output.mkdir()
    with pytest.raises(SystemExit, match="bundled fdl1 loader is missing"):
        ADAPTER.prepare_uploads(tmp_path, runtime, session, output)
    assert list(output.iterdir()) == []


@pytest.mark.parametrize("role", ["kernel", "sml", "fdl2"])
def test_uploads_cannot_overwrite_the_live_loader_stack(tmp_path: Path, role: str) -> None:
    """Reject RAM writes that reach the live stack before starting a USB transfer."""
    runtime, session, _assets = prepare_runtime(tmp_path)
    if role == "kernel":
        runtime["adapter"]["regions"][role]["load_address"] = 0x808DFFFC
    else:
        runtime["adapter"]["loader"][f"{role}_load"] = 0x808E0000
    output = tmp_path / "uploads"
    output.mkdir()
    with pytest.raises(SystemExit, match=f"{role} overlaps loader stack guard"):
        ADAPTER.prepare_uploads(tmp_path, runtime, session, output)
    assert list(output.iterdir()) == []
