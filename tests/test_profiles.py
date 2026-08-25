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
        self.platform = {
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
                "load_address": 0,
                "payload_limit": 4,
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

[[linux.copies]]
source = "host.c"
destination = "drivers/host.c"

[[linux.appends]]
source = "host.append"
destination = "drivers/Kconfig"

[rootfs]
packages = ["fplinux-host"]
exclude_packages = ["fplinux-ssh"]

[runtime]
transport = "none"
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
        self.assertEqual(before["linux"]["config_enable"], [])

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
        self.assertEqual(loaded["runtime"]["transport"], "none")

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
            config.load_profile(self.target, "usb-host")

        self._write_profile("linked")
        linked = self.target_root / "profiles" / "linked"
        (linked / "host.patch").unlink()
        (linked / "host.patch").symlink_to(profile / "host.patch")
        with (
            mock.patch.object(config, "ROOT", self.root),
            self.assertRaisesRegex(SystemExit, "profile source must not be a symlink"),
        ):
            config.load_profile(self.target, "linked")

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
