# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for profile Kconfig actions."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.build.kernel import configuration as kernel_build


class ProfileKconfigTests(unittest.TestCase):
    """Check profile actions against the resolved configuration file."""

    def test_actions_are_normalized_and_checked_against_the_resolved_file(self) -> None:
        """Only resolved enabled and disabled symbols authorize a profile."""
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / ".config"
            config.write_text("CONFIG_PROFILE_ENABLED=y\n# CONFIG_PROFILE_DISABLED is not set\n")

            self.assertEqual(
                kernel_build.profile_kconfig_arguments(
                    ["CONFIG_PROFILE_ENABLED"], ["CONFIG_PROFILE_DISABLED"]
                ),
                [
                    "--enable",
                    "PROFILE_ENABLED",
                    "--disable",
                    "PROFILE_DISABLED",
                ],
            )
            kernel_build.assert_profile_kconfig(
                config,
                ["CONFIG_PROFILE_ENABLED"],
                ["CONFIG_PROFILE_DISABLED"],
            )

            config.write_text("# CONFIG_PROFILE_ENABLED is not set\n")
            with self.assertRaisesRegex(SystemExit, "profile did not enable"):
                kernel_build.assert_profile_kconfig(config, ["CONFIG_PROFILE_ENABLED"], [])

            for value in ("y", "m"):
                with self.subTest(disabled_value=value):
                    config.write_text(f"CONFIG_PROFILE_DISABLED={value}\n")
                    with self.assertRaisesRegex(SystemExit, "profile did not disable"):
                        kernel_build.assert_profile_kconfig(
                            config,
                            [],
                            ["CONFIG_PROFILE_DISABLED"],
                        )

    def test_resolved_file_may_omit_a_disabled_symbol(self) -> None:
        """The validator accepts an omitted symbol requested to stay disabled."""
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / ".config"
            config.write_text(
                "# CONFIG_ZRAM_BACKEND_LZO is not set\n"
                "CONFIG_ZRAM_BACKEND_ZSTD=y\n"
                "CONFIG_ZRAM_DEF_COMP_ZSTD=y\n"
            )

            kernel_build.assert_profile_kconfig(
                config,
                ["CONFIG_ZRAM_BACKEND_ZSTD", "CONFIG_ZRAM_DEF_COMP_ZSTD"],
                ["CONFIG_ZRAM_BACKEND_LZO", "CONFIG_ZRAM_DEF_COMP_LZORLE"],
            )


if __name__ == "__main__":
    unittest.main()
