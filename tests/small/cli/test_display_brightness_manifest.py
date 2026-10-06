# SPDX-License-Identifier: GPL-2.0-only
"""Validate target brightness tables at the manifest boundary."""

from __future__ import annotations

import copy
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.manifests import targets


class DisplayBrightnessManifestTests:
    """Keep the selected raw levels valid for the runtime configuration."""

    @pytest.mark.parametrize(
        ("target", "name", "boot_level", "maximum"),
        [
            ("nokia-ta1618", "ta1618-backlight", 15, 20),
            ("inoi-240-modern-4g", "inoi240-backlight", 18, 31),
            ("inoi-244-modern-4g", "inoi244-backlight", 18, 31),
        ],
        ids=["nokia-ta1618", "inoi-240-modern-4g", "inoi-244-modern-4g"],
    )
    def test_phone_tables_match_published_backlights_and_boot_levels(
        self, target: str, name: str, boot_level: int, maximum: int
    ) -> None:
        """Current phones select their backlight and boot level within its raw range."""
        table = targets.load_target(target)["display_brightness"]
        assert (table["backlight"]) == (name)
        assert (table["levels"][7]) == (boot_level)
        assert (table["levels"][-1]) <= (maximum)

    def test_absent_table_remains_supported(self) -> None:
        """A headless target can omit the display configuration entirely."""
        raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
        raw.pop("display_brightness")
        with mock.patch.object(targets, "load_toml", return_value=raw):
            assert ("display_brightness") not in (targets.load_target("nokia-ta1618"))

    @pytest.mark.parametrize(
        ("table", "error"),
        [
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
        ],
        ids=[
            "device name-1",
            "device name-2",
            "exactly 11-3",
            "start at 0-4",
            "strictly ascend-5",
            "integer in 0..63-6",
            "integer in 0..63-7",
        ],
    )
    def test_invalid_table_is_rejected_before_build(
        self, table: dict[str, object], error: str
    ) -> None:
        """Reject unsafe names and malformed or non-ascending raw values."""
        original = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
        raw = copy.deepcopy(original)
        raw["display_brightness"] = table
        with (
            mock.patch.object(targets, "load_toml", return_value=raw),
            pytest.raises(SystemExit, match=error),
        ):
            targets.load_target("nokia-ta1618")
