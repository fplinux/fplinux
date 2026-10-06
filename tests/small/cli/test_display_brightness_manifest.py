# SPDX-License-Identifier: GPL-2.0-only
"""Validate target brightness tables at the manifest boundary."""

from __future__ import annotations

import copy
import unittest
from unittest import mock

from fplinux_cli import common
from fplinux_cli.manifests import targets


class DisplayBrightnessManifestTests(unittest.TestCase):
    """Keep the selected raw levels valid for the runtime configuration."""

    def test_phone_tables_match_published_backlights_and_boot_levels(self) -> None:
        """Current phones select their backlight and boot level within its raw range."""
        cases = {
            "nokia-ta1618": ("ta1618-backlight", 15, 20),
            "inoi-240-modern-4g": ("inoi240-backlight", 18, 31),
            "inoi-244-modern-4g": ("inoi244-backlight", 18, 31),
        }
        for target, (name, boot_level, maximum) in cases.items():
            with self.subTest(target=target):
                table = targets.load_target(target)["display_brightness"]
                self.assertEqual(table["backlight"], name)
                self.assertEqual(table["levels"][7], boot_level)
                self.assertLessEqual(table["levels"][-1], maximum)

    def test_absent_table_remains_supported(self) -> None:
        """A headless target can omit the display configuration entirely."""
        raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
        raw.pop("display_brightness")
        with mock.patch.object(targets, "load_toml", return_value=raw):
            self.assertNotIn("display_brightness", targets.load_target("nokia-ta1618"))

    def test_invalid_table_is_rejected_before_build(self) -> None:
        """Reject unsafe names and malformed or non-ascending raw values."""
        original = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
        invalid = (
            ({"backlight": "../screen", "levels": list(range(11))}, "device name"),
            ({"backlight": "..", "levels": list(range(11))}, "device name"),
            ({"backlight": "screen", "levels": list(range(10))}, "exactly 11"),
            ({"backlight": "screen", "levels": [1, *range(2, 12)]}, "start at 0"),
            (
                {"backlight": "screen", "levels": [0, 1, 2, 3, 4, 5, 5, 7, 8, 9, 10]},
                "strictly ascend",
            ),
            ({"backlight": "screen", "levels": [*range(10), 64]}, "integer in 0..63"),
            (
                {"backlight": "screen", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, True]},
                "integer in 0..63",
            ),
        )
        for table, error in invalid:
            with self.subTest(table=table):
                raw = copy.deepcopy(original)
                raw["display_brightness"] = table
                with (
                    mock.patch.object(targets, "load_toml", return_value=raw),
                    self.assertRaisesRegex(SystemExit, error),
                ):
                    targets.load_target("nokia-ta1618")
