# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of the SC9820E manifest consumed by RAM builds and local loaders."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.build.kernel.prepare import generated_linux_files
from fplinux_cli.build.storage.layout import root_bootargs_dtsi
from fplinux_cli.manifests.linux import discover_linux_targets
from fplinux_cli.manifests.paths import discover_profiles
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def sc_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Own the minimal platform, board and both supported shared profile declarations."""
    for relative, source in (
        ("platforms/sc9820e/platform.toml", "tests/fixtures/sc9820e-manifest/platform.toml"),
        ("targets/phone/target.toml", "tests/fixtures/sc9820e-manifest/target.toml"),
        ("profiles/default/profile.toml", "profiles/default/profile.toml"),
        ("profiles/microsd-root/profile.toml", "profiles/microsd-root/profile.toml"),
    ):
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes((ROOT / source).read_bytes())
    (tmp_path / "platforms/sc9820e/linux/overlay").mkdir(parents=True)
    (tmp_path / "targets/phone/linux/overlay").mkdir(parents=True)
    (tmp_path / "targets/phone/loader").mkdir()
    monkeypatch.setattr(common, "ROOT", tmp_path)
    return tmp_path


def test_sc_ram_target_declares_source_loaders_without_reading_firmware(
    sc_checkout: Path,
) -> None:
    """A normal build selects source loaders and publishes their three runtime asset roles."""
    platform = load_platform("sc9820e")
    config = load_target("phone")

    assert platform["bootstrap"] == {"backend": "sc9820e", "toolchain": "arm-none-eabi"}
    assert "uboot" not in platform
    assert config["bootstrap"] == {
        "backend": "sc9820e",
        "kind": "linux",
        "source": "bootstrap",
        "image": "ramboot.bin",
        "map": "ramboot.map",
        "record_prefix": "EXAMPLE_PHONE",
        "toolchain": "arm-none-eabi",
        "initramfs_delivery": "external",
        "layout": {
            "entry_load": 0x80002060,
            "entry_limit": 0x80004000,
            "kernel_load": 0x80008000,
            "kernel_stage": 0x81008000,
            "kernel_stage_limit": 0x82000000,
            "fdt_load": 0x85400000,
            "initramfs_load": 0x85500000,
            "stack": 0x9FFEFF50,
        },
        "load_address": 0x80002060,
        "payload_limit": 0x80004000,
    }
    assert config["loader_assets"] == []
    assert config["device_data"] == {"groups": {}}
    assert config["runtime"]["fdl1_load_address"] == 0x5000
    assert config["runtime"]["assets"] == {
        "fdl1": "assets/fdl1.bin",
        "fdl2": "assets/fdl2.bin",
        "sml": "assets/sml.bin",
    }
    assert config["runtime"]["adapter"] == {
        "handoff_wait_seconds": 60,
        "usb_release_wait_seconds": 10,
        "session_name": "example-phone-linux",
        "boot_instructions": "Power off and connect.",
        "loader": {
            "fdl1_load": 0x5000,
            "sml_load": 0x8E000000,
            "fdl2_load": 0x9EFFFE00,
            "stack_guard_start": 0x808E0000,
            "stack_guard_end": 0x808F0000,
        },
    }
    assert config["loader"]["build"] == {
        "source": "loader",
        "fdl1_source": "chipram",
        "fdl2_source": "u_boot",
        "sml_source": "trusted_firmware",
    }
    assert not (sc_checkout / ".cache").exists()


def test_sc_microsd_root_uses_ram_linux_and_a_real_ext4_root_without_uboot(
    sc_checkout: Path,
) -> None:
    """The board admits direct external root while retaining its unchanged RAM loader."""
    manifest = sc_checkout / "targets/phone/target.toml"
    original = manifest.read_text(encoding="utf-8")
    manifest.write_text(original + "\n[microsd]\nlinux_patches = []\n", encoding="utf-8")
    ram = load_target("phone")
    card = load_target("phone", "microsd-root")

    assert discover_profiles("phone") == ("default", "microsd-root")
    assert card["linux"]["root"] == {
        "kind": "external",
        "filesystem": "ext4",
        "wait_seconds": 10,
        "partuuid": "46504c52-02",
    }
    assert card["bootstrap"]["kind"] == "linux"
    assert card["bootstrap"]["initramfs_delivery"] == "none"
    assert card["bootstrap"]["layout"] == ram["bootstrap"]["layout"]
    assert card["runtime"] == ram["runtime"]
    assert card["device_data"] == {"groups": {}}
    assert card["uboot"] == {"kind": "none"}
    assert card["fit"] == {"kind": "none"}
    assert card["image"]["kind"] == "ext4-root"
    assert card["image"]["size"] == 512 * 1024 * 1024
    assert "CONFIG_BLK_DEV_INITRD" in card["linux"]["config_disable"]
    assert "CONFIG_EXT4_FS" in card["linux"]["config_enable"]
    bootargs = root_bootargs_dtsi(card["linux"]["root"])
    assert b"root=PARTUUID=46504c52-02 rootfstype=ext4 rootwait=10 rw init=/sbin/init" in bootargs
    assert b"linux,initrd-" not in bootargs
    assert b"rdinit=" not in bootargs
    with pytest.raises(SystemExit, match="does not support profile microsd-uboot"):
        load_target("phone", "microsd-uboot")


@pytest.mark.usefixtures("sc_checkout")
def test_sc_target_without_microsd_inputs_refuses_the_external_root_profile() -> None:
    """A removable-media profile cannot borrow another board's capability declaration."""
    assert discover_profiles("phone") == ("default",)
    with pytest.raises(SystemExit, match="does not support profile microsd-root"):
        load_target("phone", "microsd-root")


@pytest.mark.parametrize(
    ("original", "replacement", "message"),
    [
        ('backend = "sc9820e"', "", "backend must be ums9117 or sc9820e"),
        ('backend = "sc9820e"', 'backend = "unknown"', "backend must be ums9117 or sc9820e"),
        (
            'toolchain = "arm-none-eabi"',
            'toolchain = "arm-none-eabi"\nlto = 0',
            "platform bootstrap must contain exactly",
        ),
        (
            "[runtime.adapter]",
            "[runtime]\nfdl1_load_address = 0x5000\n\n[runtime.adapter]",
            "platform runtime must contain exactly",
        ),
    ],
    ids=("missing-backend", "unknown-backend", "ums-build-setting", "board-address-in-platform"),
)
def test_sc_platform_requires_its_exact_backend_contract(
    sc_checkout: Path, original: str, replacement: str, message: str
) -> None:
    """Backend selection is explicit and rejects settings owned by the other loader or board."""
    manifest = sc_checkout / "platforms/sc9820e/platform.toml"
    manifest.write_text(
        manifest.read_text(encoding="utf-8").replace(original, replacement), encoding="utf-8"
    )
    with pytest.raises(SystemExit, match=message):
        load_platform("sc9820e")


@pytest.mark.parametrize(
    ("original", "replacement", "message"),
    [
        ("entry_limit = 0x80004000", "entry_limit = 0x80009000", "ranges must be ordered"),
        ("kernel_stage = 0x81008000", "kernel_stage = 0x80008000", "ranges must be ordered"),
        (
            "kernel_stage_limit = 0x82000000",
            "kernel_stage_limit = 0x81000000",
            "ranges must be ordered",
        ),
        (
            "stack_guard_start = 0x808e0000",
            "stack_guard_start = 0x808f0000",
            "stack guard must have increasing bounds",
        ),
        (
            "stack_guard_start = 0x808e0000",
            "stack_guard_start = 0x70000000",
            "stack guard must fit inside Linux memory",
        ),
        ("stack = 0x9ffeff50", "stack = 0xa0000000", "ranges must be ordered"),
        ('source = "loader"', 'source = "../loader"', "must be a normalized relative path"),
        ('fdl1_source = "chipram"', 'fdl1_source = "../chipram"', "normalized relative path"),
        ("[adapter]\n", "[adapter]\nexec_distance = 0\n", "target adapter must contain exactly"),
        (
            'source = "loader"',
            'source = "missing"',
            "regular source directory",
        ),
    ],
    ids=(
        "entry-overlap",
        "unstaged-kernel",
        "empty-staging-arena",
        "empty-stack-guard",
        "stack-guard-outside-ram",
        "stack-outside-ram",
        "escaping-source",
        "escaping-source-lock",
        "ums-adapter",
        "missing-source-directory",
    ),
)
def test_sc_target_rejects_unloadable_layout_or_loader_records(
    sc_checkout: Path, original: str, replacement: str, message: str
) -> None:
    """Invalid RAM arenas and source selection fail before any builder or loader runs."""
    manifest = sc_checkout / "targets/phone/target.toml"
    manifest.write_text(
        manifest.read_text(encoding="utf-8").replace(original, replacement), encoding="utf-8"
    )
    with pytest.raises(SystemExit, match=message):
        load_target("phone")


def test_sc_linux_discovery_keeps_native_identity_and_default_audio_policy(
    sc_checkout: Path,
) -> None:
    """The shared source consumer discovers SC inputs without opening the full loader contract."""
    targets = discover_linux_targets(sc_checkout, {"linux": {"sha256": "a" * 64}}, "a" * 64)

    assert len(targets) == 1
    assert targets[0].config["identity"]["compatible"] == "example,phone"
    assert targets[0].platform["linux"]["arch"] == "arm"
    assert targets[0].platform["linux"]["platform_identity_header"] == (
        "arch/arm/mach-sc9820e/fplinux-platform-identity.h"
    )
    assert targets[0].config["audio_policy"] == {
        "headfree_headphones_only": False,
        "speaker_from_headphones": False,
        "speaker_vibration": False,
    }

    generated = generated_linux_files(targets[0].name, targets[0].config, targets[0].platform)

    assert not any(name.endswith("-audio-policy.dtsi") for name in generated)
    assert (
        b'compatible = "example,phone", "sprd,sc9820e";'
        in generated["arch/arm/boot/dts/unisoc/fplinux-phone-identity.dtsi"]
    )


def test_sc_target_rejects_noncanonical_dt_ownership_paths(sc_checkout: Path) -> None:
    """Board-owned requirements use the same validated exact-path contract as the platform."""
    manifest = sc_checkout / "targets/phone/target.toml"
    manifest.write_text(
        manifest.read_text(encoding="utf-8").replace(
            "[linux]\n",
            '[linux]\ndt_config_checks = [{path="/soc/../amplifier", '
            'compatible="example,amplifier", config="CONFIG_AMPLIFIER"}]\n',
        ),
        encoding="utf-8",
    )

    with pytest.raises(SystemExit, match=r"target linux dt_config_checks.*canonical absolute"):
        load_target("phone")


def test_ums_audio_policy_still_generates_the_declared_speaker_topology() -> None:
    """The existing UMS audio consumer keeps its generated vibration setting."""
    config = load_target("inoi-240-modern-4g")
    platform = load_platform("ums9117")

    generated = generated_linux_files("inoi-240-modern-4g", config, platform)
    expected = (
        # REUSE-IgnoreStart
        b"// SPDX-License-Identifier: GPL-2.0-only\n&audio {\n\tfplinux,speaker-vibration;\n};\n"
        # REUSE-IgnoreEnd
    )

    assert (
        generated["arch/arm/boot/dts/unisoc/fplinux-inoi-240-modern-4g-audio-policy.dtsi"]
        == expected
    )


@pytest.mark.parametrize("profile", ["default", "microsd-root"])
def test_named_original_manifest_preserves_its_preparer_and_payload_across_root_profiles(
    sc_checkout: Path, profile: str
) -> None:
    """Both root profiles retain the same originals boundary and declared radio payload."""
    manifest = sc_checkout / "targets/phone/target.toml"
    declaration = (ROOT / "tests/fixtures/original-files/device-data.toml").read_bytes()
    manifest.write_bytes(
        manifest.read_bytes() + b"\n" + declaration + b"\n[microsd]\nlinux_patches = []\n"
    )

    config = load_target("phone", profile)

    assert config["device_data"]["originals_parser"] == "originals_parser.py"
    assert "parser" not in config["device_data"]
    assert config["device_data"]["groups"]["wcn-firmware"][0] == {
        "source": "wcnmodem.bin",
        "destination": "wcnmodem.bin",
        "size": 3,
    }
    assert "nand" not in config


@pytest.mark.parametrize(
    ("source_declaration", "message"),
    [
        ("", "must contain exactly one of parser or originals_parser"),
        (
            'parser = "nand.py"\noriginals_parser = "originals.py"',
            "must contain exactly one of parser or originals_parser",
        ),
        ('originals_parser = "originals.txt"', "originals_parser must be a Python filename"),
        ('originals_parser = "../originals.py"', "normalized relative path"),
    ],
    ids=["missing-parser", "both-parsers", "non-python-parser", "escaping-parser"],
)
def test_named_original_manifest_rejects_ambiguous_or_invalid_source_selection(
    sc_checkout: Path, source_declaration: str, message: str
) -> None:
    """Each target selects one source boundary and a safe Python source filename."""
    manifest = sc_checkout / "targets/phone/target.toml"
    declaration = (ROOT / "tests/fixtures/original-files/device-data.toml").read_text()
    declaration = declaration.replace(
        'originals_parser = "originals_parser.py"', source_declaration
    )
    manifest.write_text(manifest.read_text() + "\n" + declaration, encoding="utf-8")

    with pytest.raises(SystemExit, match=message):
        load_target("phone")
