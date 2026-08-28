# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral tests for target-owned build-profile configuration."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import config
from fplinux_cli import workspace as workspace_module


class TargetProfileTests(unittest.TestCase):
    """Keep profile selection explicit, local and causal."""

    def setUp(self) -> None:
        """Create one complete target manifest and profile-owned source fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.target = "demo"
        self.target_root = self.root / "targets" / self.target
        self.target_root.mkdir(parents=True)
        (self.target_root / "target.toml").write_text(
            """platform = "platform"

[identity]
brand = "Demo"
product = "Phone"
hardware_codes = []
compatible = "demo,phone"

[bundle]
packages = []

[linux]
dtb = "demo.dtb"
debug_dtb = "demo.dtb"
patches = []
copies = []
appends = []
forbidden_config = ["CONFIG_FORBIDDEN=y"]
forbidden_dtb_markers = ["forbidden"]

[bootstrap]
image = "bootstrap.bin"
map = "bootstrap.map"
dtb_destination = "demo.dtb"
record_prefix = "DEMO"

[adapter]
spi_mode = 0
lcd_id = 0
exec_distance = 0
backlight_channels = "mono"
backlight_level = 0
session_name = "demo"
boot_instructions = "demo"
""",
            encoding="utf-8",
        )
        self.platform: dict[str, Any] = {
            "identity": {
                "vendor": "Demo",
                "soc": "SOC1",
                "aliases": [],
                "compatible": "demo,soc1",
                "display_name": "Demo SOC1",
            },
            "rootfs": {"packages": ["fplinux-ssh"]},
            "linux": {"copies": [{"source": "platform.c", "destination": "drivers/base.c"}]},
            "bootstrap": {
                "kernel_destination": "zImage",
                "load_address": 0x80100000,
                "payload_limit": 0x82000000,
                "layout": {
                    "ram_base": 0x80000000,
                    "ram_size": 0x04000000,
                    "timer_hz": 1000,
                    "kernel_load": 0x82000000,
                    "kernel_entry": 0x82000000,
                    "kernel_size": 0x01200000,
                    "fdt_load": 0x83E00000,
                    "fdt_size": 0x00010000,
                    "framebuffer": 0x83F00000,
                    "framebuffer_size": 0x00100000,
                },
                "toolchain": "toolchain",
                "lto": 0,
            },
            "runtime": {
                "fdl1_load_address": 0,
                "adapter": {},
                "usb": {"linux_gadget": {}},
            },
        }

    def _write_profile(self, name: str = "usb-host") -> Path:
        profile = self.target_root / "profiles" / name
        profile.mkdir(parents=True)
        (profile / "host.patch").write_text("patch\n", encoding="utf-8")
        (profile / "host.c").write_text("source\n", encoding="utf-8")
        (profile / "host.append").write_text("append\n", encoding="utf-8")
        (profile / "profile.toml").write_text(
            f"""name = "{name}"

[linux]
config_enable = ["CONFIG_USB", "CONFIG_HID"]
config_disable = ["CONFIG_USB_GADGET"]
patches = ["host.patch"]

[linux.root]
kind = "initramfs"

[[linux.copies]]
source = "host.c"
destination = "drivers/host.c"

[[linux.appends]]
source = "host.append"
destination = "drivers/Kconfig"

[rootfs]
packages = ["fplinux-host"]
exclude_packages = ["fplinux-ssh"]

[bootstrap]
kind = "linux"

[uboot]
kind = "none"

[fit]
kind = "none"

[runtime]
transport = "none"
runnable = true
""",
            encoding="utf-8",
        )
        return profile

    def _load_target(self, profile: str | None = None) -> dict[str, Any]:
        with (
            mock.patch.object(config, "ROOT", self.root),
            mock.patch.object(config, "load_platform", return_value=self.platform),
            mock.patch.object(config, "asset_bundle_paths", return_value={}),
        ):
            return config.load_target(self.target, profile)

    def test_default_target_ignores_existing_profiles(self) -> None:
        """A normal build neither selects nor hashes an unrelated profile manifest."""
        profile = self._write_profile()
        before = self._load_target()
        (profile / "profile.toml").write_text("invalid = true\n", encoding="utf-8")
        after = self._load_target()

        self.assertIsNone(before["profile"])
        self.assertEqual(before["identity"]["display_name"], "Demo Phone")
        self.assertEqual(before, after)
        self.assertEqual(before["runtime"]["transport"], "usb-ncm")
        self.assertTrue(before["runtime"]["runnable"])
        self.assertEqual(before["linux"]["config_enable"], [])
        self.assertEqual(before["linux"]["root"], {"kind": "initramfs"})
        self.assertEqual(before["bootstrap"]["kind"], "linux")
        self.assertEqual(before["uboot"], {"kind": "none"})
        self.assertEqual(before["fit"], {"kind": "none"})
        self.assertIsNone(before["layout"])
        self.assertIsNone(before["storage"])
        self.assertEqual(before["image"], {"kind": "none"})

    def test_target_rejects_a_stored_legacy_display_name(self) -> None:
        """Require public names to be derived from structured identity fields."""
        manifest = self.target_root / "target.toml"
        contents = manifest.read_text(encoding="utf-8")
        manifest.write_text(
            contents.replace(
                'platform = "platform"\n',
                'platform = "platform"\ndisplay_name = "Legacy"\n',
                1,
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(SystemExit, "must contain exactly"):
            self._load_target()

    def test_selected_profile_is_exactly_merged_under_its_target_directory(self) -> None:
        """A selected profile contributes its own operations, rootfs delta and transport."""
        self._write_profile()
        loaded = self._load_target("usb-host")

        self.assertEqual(loaded["profile"], "usb-host")
        self.assertEqual(loaded["linux"]["config_enable"], ["CONFIG_USB", "CONFIG_HID"])
        self.assertEqual(loaded["linux"]["config_disable"], ["CONFIG_USB_GADGET"])
        self.assertEqual(loaded["linux"]["patches"], ["profiles/usb-host/host.patch"])
        self.assertEqual(
            loaded["linux"]["copies"],
            [{"source": "profiles/usb-host/host.c", "destination": "drivers/host.c"}],
        )
        self.assertEqual(
            loaded["linux"]["appends"],
            [{"source": "profiles/usb-host/host.append", "destination": "drivers/Kconfig"}],
        )
        self.assertEqual(
            loaded["rootfs"],
            {"packages": ["fplinux-host"], "exclude_packages": ["fplinux-ssh"]},
        )
        self.assertEqual(loaded["linux"]["root"], {"kind": "initramfs"})
        self.assertEqual(loaded["bootstrap"]["kind"], "linux")
        self.assertEqual(loaded["uboot"], {"kind": "none"})
        self.assertEqual(loaded["fit"], {"kind": "none"})
        self.assertIsNone(loaded["layout"])
        self.assertIsNone(loaded["storage"])
        self.assertEqual(loaded["image"], {"kind": "none"})
        self.assertEqual(loaded["runtime"]["transport"], "none")
        self.assertTrue(loaded["runtime"]["runnable"])

    def test_external_root_requires_full_uboot_fit_and_matching_image(self) -> None:
        """Normalize one implemented pipeline and reject unsupported stage claims."""
        profile = self._write_profile("microsd")
        (profile / "stage0").mkdir()
        (profile / "u-boot.defconfig").write_text("CONFIG_TEST=y\n", encoding="utf-8")
        (profile / "u-boot.lock.toml").write_text(
            """version = "2026.07"
repository = "https://source.denx.de/u-boot/u-boot.git"
tag = "v2026.07"
commit = "ece349ade2973e220f524ce59e59711cc919263f"
archive_url = "https://ftp.denx.de/pub/u-boot/u-boot-2026.07.tar.bz2"
archive_sha256 = "78e8bfc382fe388f9b55aa1daf8c563522a037779b5d4c349d1415e381f1243e"
license = "GPL-2.0-only"
""",
            encoding="utf-8",
        )
        manifest = profile / "profile.toml"
        contents = manifest.read_text(encoding="utf-8")
        contents = contents.replace(
            '[linux.root]\nkind = "initramfs"',
            '[linux.root]\nkind = "external"\nfilesystem = "ext4"\nwait_seconds = 10',
        )
        contents = contents.replace(
            '[bootstrap]\nkind = "linux"',
            '[bootstrap]\nkind = "uboot-stage0"\nsource = "stage0"\n'
            'image = "stage0.bin"\nmap = "stage0.map"',
        )
        contents = contents.replace(
            '[uboot]\nkind = "none"',
            '[uboot]\nkind = "full"\nsource = "u-boot.lock.toml"\n'
            'archive_prefix = "u-boot-2026.07"\n'
            'defconfig = "u-boot.defconfig"\npatches = []\ncopies = []',
        )
        contents = contents.replace(
            '[fit]\nkind = "none"',
            '[fit]\nkind = "sha256"\nfilename = "FPLINUX.ITB"',
        )
        contents = contents.replace(
            "[runtime]",
            """[layout]
resident_start = 0x80100000
resident_limit = 0x81000000
uboot_load = 0x81000000
uboot_size = 0x00100000
uboot_stack = 0x80f00000
fit_load = 0x83200000
fit_size = 0x00c00000
fdt_pad = 0x00003000

[storage]
filename = "FPLINUX.img"
disk_signature = 0x46504c58
boot_partition = 1
boot_offset = 0x00100000
boot_size = 0x04000000
boot_label = "FPLBOOT"
root_partition = 2
root_offset = 0x04100000
root_size = 0x04000000
root_filename = "FPLROOT.ext4"
root_label = "FPLROOT"
root_uuid = "042681b5-d000-5b78-9c16-8e8b2944594e"
block_size = 4096
inode_size = 256

[runtime]""",
        )
        manifest.write_text(contents, encoding="utf-8")

        loaded = self._load_target("microsd")

        self.assertEqual(loaded["linux"]["root"]["partuuid"], "46504c58-02")
        self.assertEqual(
            loaded["linux"]["config_enable"],
            ["CONFIG_USB", "CONFIG_HID", "CONFIG_EXT4_FS"],
        )
        self.assertEqual(
            loaded["linux"]["config_disable"],
            ["CONFIG_USB_GADGET", "CONFIG_BLK_DEV_INITRD"],
        )
        self.assertEqual(loaded["uboot"]["lock"]["version"], "2026.07")
        self.assertEqual(loaded["fit"]["filename"], "FPLINUX.ITB")
        self.assertEqual(loaded["layout"]["fit_load"], 0x83200000)
        self.assertEqual(loaded["storage"]["partuuid"], "46504c58-02")
        self.assertEqual(loaded["image"]["size"], 64 * 1024 * 1024)

        manifest.write_text(contents.replace('kind = "full"', 'kind = "spl"'), encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "kind must be none or full"):
            self._load_target("microsd")

    def test_discovery_rejects_invalid_or_linked_profile_entries(self) -> None:
        """No linked or unnamed data can become a selectable profile."""
        self._write_profile()
        invalid = self.target_root / "profiles" / "Bad"
        invalid.mkdir()
        (invalid / "profile.toml").write_text("", encoding="utf-8")
        with (
            mock.patch.object(config, "ROOT", self.root),
            self.assertRaisesRegex(SystemExit, "invalid profile name"),
        ):
            config.discover_profiles(self.target)

        (invalid / "profile.toml").unlink()
        invalid.rmdir()
        linked = self.target_root / "profiles" / "linked"
        linked.symlink_to(self.target_root / "profiles" / "usb-host", target_is_directory=True)
        with (
            mock.patch.object(config, "ROOT", self.root),
            self.assertRaisesRegex(SystemExit, "profile entry is invalid"),
        ):
            config.discover_profiles(self.target)

    def test_profile_rejects_conflicting_operations_and_linked_sources(self) -> None:
        """Profiles cannot enable and disable one symbol or dereference a source link."""
        profile = self._write_profile()
        manifest = profile / "profile.toml"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                'config_disable = ["CONFIG_USB_GADGET"]',
                'config_disable = ["CONFIG_USB"]',
            ),
            encoding="utf-8",
        )
        with (
            mock.patch.object(config, "ROOT", self.root),
            self.assertRaisesRegex(SystemExit, "config_enable/config_disable conflict"),
        ):
            config.load_profile(self.target, "usb-host", self.platform["bootstrap"]["layout"])

        self._write_profile("linked")
        linked = self.target_root / "profiles" / "linked"
        (linked / "host.patch").unlink()
        (linked / "host.patch").symlink_to(profile / "host.patch")
        with (
            mock.patch.object(config, "ROOT", self.root),
            self.assertRaisesRegex(SystemExit, "profile source must not be a symlink"),
        ):
            config.load_profile(self.target, "linked", self.platform["bootstrap"]["layout"])

    def test_profile_copy_cannot_replace_an_existing_projection(self) -> None:
        """Profiles have no copy-override mode in the first profile contract."""
        profile = self._write_profile()
        (profile / "profile.toml").write_text(
            (profile / "profile.toml")
            .read_text(encoding="utf-8")
            .replace('destination = "drivers/host.c"', 'destination = "drivers/base.c"'),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(SystemExit, "copies conflict"):
            self._load_target("usb-host")

    def test_profile_rootfs_cannot_exclude_or_repeat_an_unowned_package(self) -> None:
        """Profile rootfs changes are constrained to the effective base package set."""
        profile = self._write_profile()
        manifest = profile / "profile.toml"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                'exclude_packages = ["fplinux-ssh"]',
                'exclude_packages = ["fplinux-missing"]',
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(SystemExit, "excludes a package not owned"):
            self._load_target("usb-host")

        self._write_profile("duplicate")
        duplicate = self.target_root / "profiles" / "duplicate" / "profile.toml"
        duplicate.write_text(
            duplicate.read_text(encoding="utf-8")
            .replace('packages = ["fplinux-host"]', 'packages = ["fplinux-ssh"]')
            .replace('exclude_packages = ["fplinux-ssh"]', "exclude_packages = []"),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(SystemExit, "duplicate common/platform ownership"):
            self._load_target("duplicate")

    def test_selected_workspace_is_distinct_while_default_has_no_profile_input(self) -> None:
        """Only an explicitly selected profile changes the target workspace recipe."""
        source = self.root / "source"
        source.write_bytes(b"default")
        profile_source = self.root / "profile-source"
        profile_source.write_bytes(b"profile")
        calls: list[str | None] = []

        def inventory(target: str, profile: str | None = None) -> list[tuple[str, Path]]:
            self.assertEqual(target, "demo")
            calls.append(profile)
            if profile is None:
                return [("source", source)]
            return [("source", source), ("profiles/usb-host/profile.toml", profile_source)]

        with mock.patch.object(
            workspace_module, "target_build_source_files", side_effect=inventory
        ):
            default = workspace_module.target_workspace_snapshot("demo")
            selected = workspace_module.target_workspace_snapshot("demo", "usb-host")

        self.assertEqual(calls, [None, "usb-host"])
        self.assertNotEqual(default.recipe, selected.recipe)


if __name__ == "__main__":
    unittest.main()
