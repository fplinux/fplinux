# SPDX-License-Identifier: GPL-2.0-only
"""Compatibility decisions for physical acquisition and selected preparation inputs."""

from __future__ import annotations

import copy
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.phone_data import acquisition_recipe, preparation_recipe
from fplinux_cli.build.runtime import preparation_source_files, runtime_source_files

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path


def nand_tree(*, phandle: int = 1, display: int = 100, resource: int = 0x402E0000) -> bytes:
    """Build independent NAND resources and an unrelated display node."""
    return binary_tree(
        [("#address-cells", struct.pack(">I", 1)), ("#size-cells", struct.pack(">I", 1))],
        [
            (
                "soc",
                [("#address-cells", struct.pack(">I", 1)), ("#size-cells", struct.pack(">I", 1))],
                [
                    (
                        "nand-controller@21a00000",
                        [
                            ("compatible", b"fplinux,ums9117-nandc-ro\0"),
                            ("reg", struct.pack(">II", 0x21A00000, 0x1000)),
                            ("reg-names", b"nandc\0"),
                            ("sprd,aon-apb", struct.pack(">I", phandle)),
                            ("sprd,pinmux-values", struct.pack(">I", 0)),
                        ],
                        [],
                    ),
                    (
                        "syscon@402e0000",
                        [
                            ("compatible", b"sprd,ums9117-aon-apb\0syscon\0"),
                            ("reg", struct.pack(">II", resource, 0x1000)),
                            ("phandle", struct.pack(">I", phandle)),
                        ],
                        [],
                    ),
                    ("display", [("brightness", struct.pack(">I", display))], []),
                ],
            )
        ],
    )


@dataclass(frozen=True)
class PhoneDataInputs:
    """Owned source trees and effective DTB for compatibility comparisons."""

    root: Path
    linux: Path
    dtb: Path
    config: dict[str, Any]

    def acquisition(self) -> str:
        """Identify the reader represented by these independently prepared inputs."""
        return acquisition_recipe(
            self.config, source_root=self.root, linux_source=self.linux, dtb=self.dtb
        )

    def preparation(self) -> str:
        """Identify the parser inputs represented by this selected runtime."""
        return preparation_recipe("phone", self.config, source_root=self.root)


@pytest.fixture
def phone_data_inputs(tmp_path: Path) -> PhoneDataInputs:
    """Create synthetic delivered Python and prepared C sources without a build cache."""
    config: dict[str, Any] = {
        "platform": "ums9117",
        "identity": {"compatible": "demo,phone", "display_name": "Demo Phone"},
        "linux": {"root": {"kind": "initramfs"}},
        "nand": {"raw_device": "/dev/nand-raw", "id": 0x21E5, "raw_page_bytes": 2112},
        "device_data": {
            "parser": "phone_data.py",
            "groups": {"audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}]},
        },
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        },
    }
    root = tmp_path / "source"
    for source in runtime_source_files("phone", config, root=root).values():
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"def prepare():\n    return b'phone'\n")
    linux = tmp_path / "linux"
    for relative, content in (
        ("drivers/memory/ums9117/ums9117-nandc-ro.c", b"int read_page(void) { return 0; }\n"),
        ("drivers/memory/ums9117/ums9117-nandc-chip.c", b"int chip(void) { return 0; }\n"),
        ("drivers/memory/ums9117/ums9117-nandc-chip.h", b"int chip(void);\n"),
        ("drivers/memory/ums9117/ums9117-nandc-feature.c", b"int feature(void) { return 0; }\n"),
        ("drivers/memory/ums9117/ums9117-nandc-feature.h", b"int feature(void);\n"),
        ("include/linux/soc/ums9117/mmio.h", b"#define READ_REG(x) (*(x))\n"),
    ):
        source = linux / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(content)
    dtb = tmp_path / "phone.dtb"
    dtb.write_bytes(nand_tree())
    return PhoneDataInputs(root, linux, dtb, config)


def test_acquisition_reuses_backup_after_unrelated_build_and_display_changes(
    phone_data_inputs: PhoneDataInputs,
) -> None:
    """A display, release, preparation or transport edit cannot require another raw read."""
    before = phone_data_inputs.acquisition()
    phone_data_inputs.config["release_id"] = "another-release"
    phone_data_inputs.config["runtime"] = {"transport": "usb-ncm", "timeout": 60}
    (phone_data_inputs.root / "scripts/fplinux_cli/runtime/ssh_transport.py").write_bytes(
        b"def connect():\n    return 'new transport'\n"
    )
    phone_data_inputs.dtb.write_bytes(nand_tree(display=200))
    parser = phone_data_inputs.root / "targets/phone/phone_data.py"
    parser.write_bytes(b"def prepare():\n    return b'updated'\n")
    (phone_data_inputs.root / "scripts/fplinux_cli/device_data/formats.py").write_bytes(
        b"def parse_partition():\n    return b'updated'\n"
    )
    assert phone_data_inputs.acquisition() == before


def test_acquisition_reuses_backup_when_unrelated_nodes_renumber_phandles(
    phone_data_inputs: PhoneDataInputs,
) -> None:
    """The resource reference identifies its node rather than the DT compiler's number."""
    before = phone_data_inputs.acquisition()
    phone_data_inputs.dtb.write_bytes(nand_tree(phandle=40))
    assert phone_data_inputs.acquisition() == before


@pytest.mark.parametrize(
    "change", ["reader", "geometry", "kernel", "chip", "chip-contract", "resource", "declaration"]
)
def test_acquisition_rejects_backup_after_reader_contract_changes(
    phone_data_inputs: PhoneDataInputs, change: str
) -> None:
    """Each changed input can alter physical bytes or the chip admitted for the target."""
    before = phone_data_inputs.acquisition()
    if change == "reader":
        path = phone_data_inputs.root / "scripts/fplinux_cli/runtime/nand_backup.py"
        path.write_bytes(b"def read_raw():\n    return b'changed'\n")
    elif change == "geometry":
        path = phone_data_inputs.root / "scripts/fplinux_cli/device_data/nand_geometry.py"
        path.write_bytes(b"PAGE_MAIN_BYTES = 4096\n")
    elif change == "kernel":
        path = phone_data_inputs.linux / "drivers/memory/ums9117/ums9117-nandc-ro.c"
        path.write_bytes(b"int read_page(void) { return 1; }\n")
    elif change == "chip":
        path = phone_data_inputs.linux / "drivers/memory/ums9117/ums9117-nandc-chip.c"
        path.write_bytes(b"int chip(void) { return 1; }\n")
    elif change == "chip-contract":
        path = phone_data_inputs.linux / "drivers/memory/ums9117/ums9117-nandc-chip.h"
        path.write_bytes(b"long chip(void);\n")
    elif change == "resource":
        phone_data_inputs.dtb.write_bytes(nand_tree(resource=0x402F0000))
    else:
        phone_data_inputs.config["nand"]["raw_page_bytes"] = 2176
    assert phone_data_inputs.acquisition() != before


@pytest.mark.parametrize("change", ["parser", "audio", "groups", "interpretation", "compatible"])
def test_preparation_changes_without_invalidating_raw_backup(
    phone_data_inputs: PhoneDataInputs, change: str
) -> None:
    """New derived output is prepared from the same raw acquisition when its policy changes."""
    acquisition = phone_data_inputs.acquisition()
    preparation = phone_data_inputs.preparation()
    if change == "parser":
        (phone_data_inputs.root / "targets/phone/phone_data.py").write_bytes(b"NEW_OUTPUT = 1\n")
    elif change == "audio":
        phone_data_inputs.config["audio_policy"]["speaker_vibration"] = True
    elif change == "groups":
        groups = copy.deepcopy(phone_data_inputs.config["device_data"]["groups"])
        groups["audio-profile"][0]["destination"] = "new-audio.bin"
        phone_data_inputs.config["device_data"]["groups"] = groups
    elif change == "interpretation":
        (phone_data_inputs.root / "scripts/fplinux_cli/device_data/formats.py").write_bytes(
            b"def partition():\n    return 1\n"
        )
    else:
        phone_data_inputs.config["identity"]["compatible"] = "demo,another-phone"
    assert phone_data_inputs.preparation() != preparation
    assert phone_data_inputs.acquisition() == acquisition


@pytest.mark.parametrize("platform", ["ums9117", "sc9820e"])
def test_explicit_preparation_uses_named_originals_without_ram_runtime_descriptor(
    phone_data_inputs: PhoneDataInputs, platform: str
) -> None:
    """External-root and named-original users retain causal preparation identities."""
    config = phone_data_inputs.config
    config["platform"] = platform
    config["linux"]["root"]["kind"] = "external"
    config["device_data"] = {
        "originals_parser": "originals.py",
        "groups": {"wcn-firmware": [{"source": "wcn.bin", "destination": "wcn.bin"}]},
    }
    config.pop("nand")
    for source in preparation_source_files("phone", config, root=phone_data_inputs.root).values():
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"def prepare_from_originals():\n    return b'original'\n")
    before = phone_data_inputs.preparation()
    (phone_data_inputs.root / "targets/phone/originals.py").write_bytes(
        b"def prepare_from_originals():\n    return b'updated'\n"
    )
    assert phone_data_inputs.preparation() != before


def test_preparation_ignores_human_identity_and_boot_instructions(
    phone_data_inputs: PhoneDataInputs,
) -> None:
    """Display names and loader text cannot change interpreted phone data."""
    before = phone_data_inputs.preparation()
    phone_data_inputs.config["identity"]["display_name"] = "New Phone Name"
    phone_data_inputs.config["runtime"] = {"boot_instructions": "Connect the phone."}
    assert phone_data_inputs.preparation() == before
