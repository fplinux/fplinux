# SPDX-License-Identifier: GPL-2.0-only
"""Causal reuse decisions for eMMC originals and their delivered preparation inputs."""

from __future__ import annotations

import shutil
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.phone_data import acquisition_recipe, preparation_recipe
from fplinux_cli.build.runtime import runtime_source_files

from tests import ROOT
from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path


def emmc_tree(*, handle: int = 1, voltage: int = 3000000, display: int = 10) -> bytes:
    """Represent independent reader, clock and regulator nodes with unrelated display data."""
    return binary_tree(
        [("#address-cells", struct.pack(">I", 1)), ("#size-cells", struct.pack(">I", 1))],
        [
            ("aliases", [("mmc1", b"/soc/mmc@20600000\0")], []),
            (
                "soc",
                [("#address-cells", struct.pack(">I", 1)), ("#size-cells", struct.pack(">I", 1))],
                [
                    (
                        "mmc@20600000",
                        [
                            ("compatible", b"fplinux,sc9820e-emmc-ro\0sprd,sdhc-r10\0"),
                            ("reg", struct.pack(">II", 0x20600000, 0x1000)),
                            ("clocks", struct.pack(">II", handle, 0)),
                            ("vmmc-supply", struct.pack(">I", handle + 1)),
                            ("non-removable", b""),
                        ],
                        [],
                    ),
                    (
                        "clock@21500090",
                        [
                            ("compatible", b"fplinux,sc9820e-emmc-clock\0"),
                            ("reg", struct.pack(">II", 0x21500090, 4)),
                            ("#clock-cells", struct.pack(">I", 1)),
                            ("phandle", struct.pack(">I", handle)),
                        ],
                        [],
                    ),
                    (
                        "regulators",
                        [("compatible", b"sprd,sc2721-regulator\0")],
                        [
                            (
                                "ldo@10",
                                [
                                    ("reg", struct.pack(">I", 10)),
                                    ("regulator-min-microvolt", struct.pack(">I", voltage)),
                                    ("regulator-max-microvolt", struct.pack(">I", voltage)),
                                    ("phandle", struct.pack(">I", handle + 1)),
                                ],
                                [],
                            )
                        ],
                    ),
                    ("display", [("brightness", struct.pack(">I", display))], []),
                ],
            ),
        ],
    )


@dataclass(frozen=True)
class EmmcInputs:
    """Own a synthetic effective kernel and source closure without a shared build cache."""

    root: Path
    linux: Path
    dtb: Path
    config: dict[str, Any]

    def acquisition(self) -> str:
        """Identify the selected physical read operation."""
        return acquisition_recipe(
            self.config, source_root=self.root, linux_source=self.linux, dtb=self.dtb
        )

    def preparation(self, *, delivered_root: Path | None = None) -> str:
        """Identify preparation with an explicit separate build-only catalog owner."""
        return preparation_recipe(
            "phone",
            self.config,
            source_root=self.root if delivered_root is None else delivered_root,
            source_catalog_root=self.root,
        )


@pytest.fixture
def emmc_inputs(tmp_path: Path) -> EmmcInputs:
    """Create source bytes whose relevant and unrelated changes can be compared."""
    config: dict[str, Any] = {
        "platform": "sc9820e",
        "identity": {"compatible": "demo,phone"},
        "linux": {"root": {"kind": "initramfs"}},
        "emmc": {
            "raw_device": "/dev/mmcblk1",
            "files": [
                {
                    "name": "audio_para",
                    "locations": [["system", "/etc/audio_para"]],
                    "max_bytes": 100,
                }
            ],
        },
        "device_data": {
            "originals_parser": "phone_data.py",
            "groups": {
                "audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}],
                "wcn-firmware": [{"source": "wcn.bin", "destination": "wcn.bin"}],
            },
            "required_groups": ["audio-profile"],
        },
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        },
    }
    root = tmp_path / "source"
    resource = root / "targets/phone/device-data/rf-fields.tsv"
    resource.parent.mkdir(parents=True)
    resource.write_bytes(b"kind\tkey\toffset\twidth\tcount\n")
    for source in runtime_source_files("phone", config, root=root).values():
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"SOURCE = 'original'\n")
    projector = root / "scripts/fplinux_cli/build/host_tools/phone_data.py"
    projector.parent.mkdir(parents=True)
    projector.write_bytes(b"def project():\n    return 'original'\n")
    (root / "sources.lock.toml").write_bytes((ROOT / "sources.lock.toml").read_bytes())
    linux = tmp_path / "linux"
    for relative in (
        "drivers/mmc/host/sdhci.c",
        "drivers/mmc/host/sdhci.h",
        "drivers/mmc/host/sdhci-pltfm.c",
        "drivers/mmc/host/sdhci-pltfm.h",
        "drivers/mmc/host/sdhci-sprd.c",
        "drivers/mmc/host/sdhci-sprd-emmc-ro.h",
        "drivers/mmc/core/block.c",
        "include/linux/mmc/core.h",
        "fs/ext4/inode.c",
        "drivers/clk/sprd/sc9820e-sdio-clock.c",
        "drivers/regulator/sc2721-regulator.c",
        "drivers/spi/spi-sprd-adi.c",
        "drivers/hwspinlock/sprd_hwspinlock.c",
        "block/partitions/efi.c",
    ):
        source = linux / relative
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(b"int original;\n")
    dtb = tmp_path / "phone.dtb"
    dtb.write_bytes(emmc_tree())
    return EmmcInputs(root, linux, dtb, config)


def test_emmc_originals_survive_unrelated_display_and_preparation_changes(
    emmc_inputs: EmmcInputs,
) -> None:
    before = emmc_inputs.acquisition()
    emmc_inputs.dtb.write_bytes(emmc_tree(handle=40, display=20))
    (emmc_inputs.root / "targets/phone/phone_data.py").write_bytes(b"SOURCE = 'new parser'\n")
    emmc_inputs.config["audio_policy"]["speaker_vibration"] = True
    assert emmc_inputs.acquisition() == before


@pytest.mark.parametrize(
    "change",
    [
        "collector",
        "child-reader",
        "projection",
        "pin",
        "mmc",
        "clock",
        "regulator",
        "voltage",
        "selection",
    ],
)
def test_emmc_originals_miss_after_acquisition_behavior_changes(
    emmc_inputs: EmmcInputs, change: str
) -> None:
    before = emmc_inputs.acquisition()
    paths = {
        "collector": emmc_inputs.root / "scripts/fplinux_cli/runtime/emmc-files/collect.sh",
        "child-reader": emmc_inputs.root
        / "scripts/fplinux_cli/device_data/emmc-files/read_files.py",
        "projection": emmc_inputs.root / "scripts/fplinux_cli/build/host_tools/phone_data.py",
        "mmc": emmc_inputs.linux / "drivers/mmc/core/block.c",
        "clock": emmc_inputs.linux / "drivers/clk/sprd/sc9820e-sdio-clock.c",
        "regulator": emmc_inputs.linux / "drivers/regulator/sc2721-regulator.c",
    }
    if change in paths:
        paths[change].write_bytes(b"changed acquisition behavior\n")
    elif change == "voltage":
        emmc_inputs.dtb.write_bytes(emmc_tree(voltage=2800000))
    elif change == "pin":
        lock = emmc_inputs.root / "sources.lock.toml"
        lock.write_text(lock.read_text().replace('version = "1.4.1"', 'version = "1.4.2"'))
    else:
        emmc_inputs.config["emmc"]["files"][0]["max_bytes"] = 101
    assert emmc_inputs.acquisition() != before


@pytest.mark.parametrize("change", ["parser", "resource", "reader", "projection", "pin"])
def test_emmc_preparation_misses_after_admitted_input_behavior_changes(
    emmc_inputs: EmmcInputs, change: str
) -> None:
    before = emmc_inputs.preparation()
    paths = {
        "parser": "targets/phone/phone_data.py",
        "resource": "targets/phone/device-data/rf-fields.tsv",
        "reader": "scripts/fplinux_cli/device_data/emmc-files/read_files.py",
        "projection": "scripts/fplinux_cli/build/host_tools/phone_data.py",
    }
    if change in paths:
        (emmc_inputs.root / paths[change]).write_bytes(b"changed preparation behavior\n")
    else:
        lock = emmc_inputs.root / "sources.lock.toml"
        lock.write_text(lock.read_text().replace('version = "1.4.1"', 'version = "1.4.2"'))
    assert emmc_inputs.preparation() != before


def test_emmc_preparation_ignores_unselected_catalog_records(emmc_inputs: EmmcInputs) -> None:
    before = emmc_inputs.preparation()
    acquisition = emmc_inputs.acquisition()
    lock = emmc_inputs.root / "sources.lock.toml"
    with lock.open("a") as output:
        output.write('\n[unrelated_test_source]\nversion = "unrelated"\n')
    assert emmc_inputs.preparation() == before
    assert emmc_inputs.acquisition() == acquisition


def test_delivered_preparation_uses_explicit_captured_catalog_without_shipping_it(
    emmc_inputs: EmmcInputs, tmp_path: Path
) -> None:
    before = emmc_inputs.preparation()
    delivered = tmp_path / "delivered"
    shutil.copytree(emmc_inputs.root / "targets", delivered / "targets")
    shutil.copytree(emmc_inputs.root / "scripts/fplinux_cli", delivered / "scripts/fplinux_cli")
    assert not (delivered / "sources.lock.toml").exists()
    assert emmc_inputs.preparation(delivered_root=delivered) == before
