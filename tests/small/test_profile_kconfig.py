# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for profile Kconfig actions."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import builder


class ProfileKconfigTests(unittest.TestCase):
    """Check profile actions against the resolved configuration file."""

    def test_actions_are_normalized_and_must_survive_olddefconfig(self) -> None:
        """Only resolved enabled and disabled symbols authorize a profile."""
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / ".config"
            config.write_text("CONFIG_PROFILE_ENABLED=y\n# CONFIG_PROFILE_DISABLED is not set\n")

            self.assertEqual(
                builder.profile_kconfig_arguments(
                    ["CONFIG_PROFILE_ENABLED"], ["CONFIG_PROFILE_DISABLED"]
                ),
                [
                    "--enable",
                    "PROFILE_ENABLED",
                    "--disable",
                    "PROFILE_DISABLED",
                ],
            )
            builder.assert_profile_kconfig(
                config,
                ["CONFIG_PROFILE_ENABLED"],
                ["CONFIG_PROFILE_DISABLED"],
            )

            config.write_text("# CONFIG_PROFILE_ENABLED is not set\n")
            with self.assertRaisesRegex(SystemExit, "profile did not enable"):
                builder.assert_profile_kconfig(config, ["CONFIG_PROFILE_ENABLED"], [])


if __name__ == "__main__":
    unittest.main()
