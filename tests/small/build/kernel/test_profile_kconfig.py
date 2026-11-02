# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for profile Kconfig actions."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.kernel import configuration as kernel_build

if TYPE_CHECKING:
    from pathlib import Path


class ProfileKconfigTests:
    """Check profile actions against the resolved configuration file."""

    @staticmethod
    @pytest.mark.parametrize(
        "disabled_value",
        [pytest.param("y", id="enabled"), pytest.param("m", id="module")],
    )
    def test_actions_are_normalized_and_checked_against_the_resolved_file(
        tmp_path: Path, disabled_value: str
    ) -> None:
        """Only resolved enabled and disabled symbols authorize a profile."""
        config = tmp_path / ".config"
        config.write_text("CONFIG_PROFILE_ENABLED=y\n# CONFIG_PROFILE_DISABLED is not set\n")

        assert (
            kernel_build.profile_kconfig_arguments(
                ["CONFIG_PROFILE_ENABLED"], ["CONFIG_PROFILE_DISABLED"]
            )
        ) == (
            [
                "--enable",
                "PROFILE_ENABLED",
                "--disable",
                "PROFILE_DISABLED",
            ]
        )
        kernel_build.assert_profile_kconfig(
            config,
            ["CONFIG_PROFILE_ENABLED"],
            ["CONFIG_PROFILE_DISABLED"],
        )

        config.write_text("# CONFIG_PROFILE_ENABLED is not set\n")
        with pytest.raises(SystemExit, match="profile did not enable"):
            kernel_build.assert_profile_kconfig(config, ["CONFIG_PROFILE_ENABLED"], [])

        config.write_text(f"CONFIG_PROFILE_DISABLED={disabled_value}\n")
        with pytest.raises(SystemExit, match="profile did not disable"):
            kernel_build.assert_profile_kconfig(
                config,
                [],
                ["CONFIG_PROFILE_DISABLED"],
            )

    @staticmethod
    def test_resolved_file_may_omit_a_disabled_symbol(tmp_path: Path) -> None:
        """The validator accepts an omitted symbol requested to stay disabled."""
        config = tmp_path / ".config"
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
