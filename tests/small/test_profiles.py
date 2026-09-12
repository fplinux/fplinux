# SPDX-License-Identifier: GPL-2.0-only
"""Global boot-policy selection and board configuration composition."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import builder, config
from fplinux_cli.common import ROOT


class GlobalProfileTests(unittest.TestCase):
    """Keep target features common while changing the system root."""

    def setUp(self) -> None:
        """Provide two board manifests and real global boot-policy inputs."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(ROOT / "profiles", self.root / "profiles")
        self.platform: dict[str, Any] = {
            "identity": {"compatible": "demo,soc"},
            "rootfs": {"packages": ["fplinux-ssh", "fplinux-feature"]},
            "linux": {"defconfig": "platforms/demo/kernel/defconfig"},
            "uboot": {
                "source": "platforms/demo/uboot.lock.toml",
                "archive_prefix": "u-boot-2026.07",
                "patches": [],
                "copies": [],
            },
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
            "runtime": {"fdl1_load_address": 0, "adapter": {}, "usb": {}},
        }
        shared = self.root / "platforms/demo"
        (shared / "kernel").mkdir(parents=True)
        (shared / "kernel/defconfig").write_text("CONFIG_FEATURE=y\nCONFIG_BOARD=1\n")
        (shared / "uboot.lock.toml").write_text(
            'version = "2026.07"\nrepository = "https://example.invalid/u-boot"\n'
            'tag = "v2026.07"\ncommit = "' + "a" * 40 + '"\n'
            'archive_url = "https://example.invalid/u-boot.tar.bz2"\n'
            'archive_sha256 = "' + "b" * 64 + '"\nlicense = "GPL-2.0-only"\n'
        )
        for target in ("first", "second"):
            target_root = self.root / "targets" / target
            (target_root / "kernel").mkdir(parents=True)
            (target_root / "kernel/config.fragment").write_text("CONFIG_BOARD=2\n")
            (target_root / "bootstrap-microsd").mkdir()
            (target_root / "uboot").mkdir()
            (target_root / "uboot/defconfig").write_text("CONFIG_ARM=y\n")
            (target_root / "target.toml").write_text(
                f"""platform = "demo"
[identity]
brand = "Demo"
product = "{target}"
hardware_codes = []
compatible = "demo,{target}"
[rootfs]
packages = []
[bundle]
packages = []
[linux]
config_fragment = "kernel/config.fragment"
memory = {{ base = 0x80200000, size = 0x03c00000 }}
dtb = "{target}.dtb"
debug_dtb = "{target}.dtb"
patches = []
copies = []
appends = []
forbidden_config = ["CONFIG_FORBIDDEN=y"]
forbidden_dtb_markers = ["forbidden"]
[bootstrap]
image = "bootstrap.bin"
map = "bootstrap.map"
dtb_destination = "{target}.dtb"
record_prefix = "DEMO"
[adapter]
spi_mode = 0
lcd_id = 0
exec_distance = 0
backlight_channels = "mono"
backlight_level = 0
session_name = "{target}"
boot_instructions = "Power off and connect."
[microsd]
linux_patches = []
[microsd.bootstrap]
source = "bootstrap-microsd"
image = "sd-stage0.bin"
map = "sd-stage0.map"
[microsd.uboot]
defconfig = "uboot/defconfig"
patches = []
copies = []
[bluetooth]
parser = "radio_parser.py"
[[bluetooth.firmware]]
source = "radio.bin"
destination = "demo/radio.bin"
size = 4
"""
            )
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(config, "ROOT", self.root).start()
        mock.patch.object(config, "load_platform", return_value=self.platform).start()
        mock.patch.object(config, "asset_bundle_paths", return_value={}).start()

    def test_default_alias_has_the_same_identity_and_configuration(self) -> None:
        """Spelling default explicitly does not create a second build context."""
        for target in ("first", "second"):
            with self.subTest(target=target):
                implicit = config.load_target(target)
                explicit = config.load_target(target, "default")
                self.assertEqual(implicit, explicit)
                self.assertIsNone(explicit["profile"])
                self.assertEqual(explicit["linux"]["root"], {"kind": "initramfs"})
                self.assertEqual(explicit["bootstrap"]["kind"], "linux")

    def test_both_boards_share_boot_policy_and_keep_their_own_boot_sources(self) -> None:
        """MicroSD changes root placement but keeps feature and firmware inputs."""
        for target in ("first", "second"):
            with self.subTest(target=target):
                ram = config.load_target(target)
                card = config.load_target(target, "microsd-uboot")
                self.assertEqual(config.discover_profiles(target), ("default", "microsd-uboot"))
                self.assertEqual(card["profile"], "microsd-uboot")
                self.assertEqual(
                    card["linux"]["root"],
                    {
                        "kind": "external",
                        "filesystem": "ext4",
                        "wait_seconds": 10,
                        "partuuid": "46504c58-02",
                    },
                )
                self.assertEqual(card["rootfs"]["packages"], ["fplinux-microsd-root"])
                self.assertEqual(ram["rootfs"]["packages"], [])
                self.assertEqual(card["rootfs"]["base_packages"], ram["rootfs"]["base_packages"])
                self.assertEqual(
                    card["rootfs"]["firmware"],
                    [
                        {
                            "source": "radio.bin",
                            "destination": "demo/radio.bin",
                            "size": 4,
                        }
                    ],
                )
                self.assertEqual(card["rootfs"]["firmware"], ram["rootfs"]["firmware"])
                self.assertEqual(card["linux"]["memory"], {"base": 0x80200000, "size": 0x03C00000})
                self.assertEqual(card["linux"]["config_fragment"], ram["linux"]["config_fragment"])
                self.assertEqual(card["bootstrap"]["source"], "bootstrap-microsd")
                self.assertEqual(card["uboot"]["defconfig"], "uboot/defconfig")
                self.assertEqual(card["runtime"], ram["runtime"])

    def test_feature_profiles_are_unavailable_even_if_old_directories_exist(self) -> None:
        """An obsolete board-local profile cannot change a global selection."""
        previous = self.root / "targets/first/profiles/bt-qual"
        previous.mkdir(parents=True)
        (previous / "profile.toml").write_text("invalid = true\n")
        self.assertEqual(config.discover_profiles("first"), ("default", "microsd-uboot"))
        with self.assertRaisesRegex(SystemExit, "unknown profile"):
            config.load_target("first", "bt-qual")
        self.assertEqual(config.load_target("first")["linux"]["root"], {"kind": "initramfs"})

    def test_boot_profile_cannot_exclude_shared_features(self) -> None:
        """Feature-package ownership is rejected at the boot-policy boundary."""
        path = self.root / "profiles/default/profile.toml"
        path.write_text(path.read_text().replace("packages = []", 'packages = ["feature"]'))
        with self.assertRaisesRegex(SystemExit, "boot maintenance"):
            config.load_target("first")

    def test_missing_bluetooth_declaration_does_not_require_private_inputs(self) -> None:
        """A board without a Bluetooth declaration has no firmware input group."""
        path = self.root / "targets/first/target.toml"
        path.write_text(path.read_text().split("[bluetooth]")[0])
        for profile in ("default", "microsd-uboot"):
            with self.subTest(profile=profile):
                self.assertEqual(config.load_target("first", profile)["rootfs"]["firmware"], [])

    def test_missing_board_boot_sources_fail_only_when_microsd_is_selected(self) -> None:
        """RAM operation does not read unused U-Boot sources or fall back to another board."""
        (self.root / "targets/second/uboot/defconfig").unlink()
        self.assertEqual(config.load_target("second")["bootstrap"]["kind"], "linux")
        with self.assertRaisesRegex(SystemExit, "U-Boot source is missing"):
            config.load_target("second", "microsd-uboot")

    def test_uboot_combines_shared_sources_with_the_selected_board(self) -> None:
        """Shared boot logic and the chosen slot descriptor reach one projection."""
        shared = self.root / "platforms/demo"
        (shared / "boot.c").write_text("shared boot flow\n")
        (shared / "boot.patch").write_text("shared integration patch\n")
        self.platform["uboot"]["patches"] = ["platforms/demo/boot.patch"]
        self.platform["uboot"]["copies"] = [
            {"source": "platforms/demo/boot.c", "destination": "board/demo/boot.c"}
        ]
        for target in ("first", "second"):
            target_root = self.root / "targets" / target
            (target_root / "uboot/slot.c").write_text(f"{target} slot\n")
            (target_root / "uboot/board.patch").write_text(f"{target} integration patch\n")
            manifest = target_root / "target.toml"
            manifest.write_text(
                manifest.read_text().replace(
                    'defconfig = "uboot/defconfig"\npatches = []\ncopies = []',
                    'defconfig = "uboot/defconfig"\npatches = ["uboot/board.patch"]\n'
                    'copies = [{ source = "targets/'
                    + target
                    + '/uboot/slot.c", destination = "board/demo/slot.c" }]',
                )
            )

            with self.subTest(target=target):
                uboot = config.load_target(target, "microsd-uboot")["uboot"]
                self.assertEqual(
                    [(self.root / path).read_text() for path in uboot["patches"]],
                    ["shared integration patch\n", f"{target} integration patch\n"],
                )
                projected = {
                    step["destination"]: (self.root / step["source"]).read_text()
                    for step in uboot["copies"]
                }
                self.assertEqual(
                    projected,
                    {
                        "board/demo/boot.c": "shared boot flow\n",
                        "board/demo/slot.c": f"{target} slot\n",
                    },
                )

    def test_nand_reader_stays_board_owned_across_boot_profiles(self) -> None:
        """Boot policy preserves an explicit reader and supplies none to other boards."""
        path = self.root / "targets/first/target.toml"
        path.write_text(
            path.read_text()
            + '\n[nand]\nraw_device = "/dev/first-nand-raw"\nid = 0xb1a1\nraw_page_bytes = 2176\n'
        )
        for profile in ("default", "microsd-uboot"):
            with self.subTest(profile=profile):
                self.assertEqual(
                    config.load_target("first", profile)["nand"],
                    {"raw_device": "/dev/first-nand-raw", "id": 0xB1A1, "raw_page_bytes": 2176},
                )
                self.assertNotIn("nand", config.load_target("second", profile))

    def test_nand_reader_requires_a_device_path(self) -> None:
        """A board cannot accidentally point a physical NAND backup at a regular file."""
        path = self.root / "targets/first/target.toml"
        path.write_text(
            path.read_text()
            + '\n[nand]\nraw_device = "/tmp/nand.raw"\nid = 0xb1a1\nraw_page_bytes = 2176\n'
        )
        with self.assertRaisesRegex(SystemExit, "device directly under /dev"):
            config.load_target("first")


class KernelConfigCompositionTests(unittest.TestCase):
    """The shared base and board fragment produce one unambiguous Kconfig input."""

    def test_board_values_override_the_base_without_losing_shared_features(self) -> None:
        """Both enabled and explicitly disabled board settings replace base values."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary) / "base"
            fragment = Path(temporary) / "fragment"
            base.write_text("CONFIG_SHARED=y\nCONFIG_BOARD=1\nCONFIG_UNUSED=y\n")
            fragment.write_text("CONFIG_BOARD=2\n# CONFIG_UNUSED is not set\nCONFIG_DEVICE=y\n")
            self.assertEqual(
                config.compose_kernel_config(base, fragment),
                (
                    b"CONFIG_SHARED=y\nCONFIG_BOARD=2\n"
                    b"# CONFIG_UNUSED is not set\nCONFIG_DEVICE=y\n"
                ),
            )


class RepositoryProfileCompressionTests(unittest.TestCase):
    """Admit prepared Kconfig files only for the selected compressor policy."""

    def test_each_profile_rejects_the_opposite_prepared_compressor(self) -> None:
        """RAM admits ZSTD and microSD admits LZO-RLE for every target."""
        prepared_configs = {
            "default": {
                "correct": (
                    "CONFIG_ZRAM=y\n"
                    "# CONFIG_ZRAM_BACKEND_LZO is not set\n"
                    "CONFIG_ZRAM_BACKEND_ZSTD=y\n"
                    "CONFIG_ZRAM_DEF_COMP_ZSTD=y\n"
                ),
                "opposite": (
                    "CONFIG_ZRAM=y\n"
                    "CONFIG_ZRAM_BACKEND_LZO=y\n"
                    "# CONFIG_ZRAM_BACKEND_ZSTD is not set\n"
                    "CONFIG_ZRAM_DEF_COMP_LZORLE=y\n"
                ),
            },
            "microsd-uboot": {
                "correct": (
                    "# CONFIG_BLK_DEV_INITRD is not set\n"
                    "CONFIG_EXT4_FS=y\n"
                    "CONFIG_ZRAM=y\n"
                    "CONFIG_ZRAM_BACKEND_LZO=y\n"
                    "# CONFIG_ZRAM_BACKEND_ZSTD is not set\n"
                    "CONFIG_ZRAM_DEF_COMP_LZORLE=y\n"
                ),
                "opposite": (
                    "# CONFIG_BLK_DEV_INITRD is not set\n"
                    "CONFIG_EXT4_FS=y\n"
                    "CONFIG_ZRAM=y\n"
                    "# CONFIG_ZRAM_BACKEND_LZO is not set\n"
                    "CONFIG_ZRAM_BACKEND_ZSTD=y\n"
                    "CONFIG_ZRAM_DEF_COMP_ZSTD=y\n"
                ),
            },
        }

        with tempfile.TemporaryDirectory() as temporary:
            prepared = Path(temporary) / ".config"
            for target in config.discover_targets():
                for profile, contents in prepared_configs.items():
                    with self.subTest(target=target, profile=profile):
                        linux = config.load_target(target, profile)["linux"]
                        prepared.write_text(contents["correct"])
                        builder.assert_profile_kconfig(
                            prepared,
                            linux["config_enable"],
                            linux["config_disable"],
                        )

                        prepared.write_text(contents["opposite"])
                        with self.assertRaisesRegex(
                            SystemExit, "profile did not (enable|disable)"
                        ):
                            builder.assert_profile_kconfig(
                                prepared,
                                linux["config_enable"],
                                linux["config_disable"],
                            )


if __name__ == "__main__":
    unittest.main()
