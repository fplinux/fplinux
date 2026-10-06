# SPDX-License-Identifier: GPL-2.0-only
"""Public CLI help tests for boot selectors and diagnostic tools."""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
_PUBLIC_HELP_TIMEOUT_SECONDS = 10


class ProfileAndToolCliHelpWorkflowTests:
    """Exercise the repository CLI without resolving a bundle or touching USB."""

    @pytest.mark.parametrize(
        "command",
        [
            pytest.param(("build",), id="build"),
            pytest.param(("check",), id="check"),
            pytest.param(("run",), id="run"),
            pytest.param(("package",), id="package"),
            pytest.param(("console",), id="console"),
            pytest.param(("verify",), id="verify"),
            pytest.param(("inspect", "bundle"), id="inspect-bundle"),
            pytest.param(("nand", "identify"), id="nand-identify"),
            pytest.param(("nand", "backup"), id="nand-backup"),
        ],
    )
    def test_help_exposes_the_build_type_enum_for_bundle_consumers(
        self, command: tuple[str, ...]
    ) -> None:
        """The documented selector is available at every public type-sensitive entry point."""
        result = run_process(
            [str(ROOT / "fplinux"), *command, "--help"],
            name="build type help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("--build-type {release,debug}") in (result.stdout)

    @pytest.mark.parametrize("command", ["run", "package"], ids=["run", "package"])
    def test_help_exposes_the_microsd_boot_alias_and_global_profile_selector(
        self, command: str
    ) -> None:
        """Run and package expose both supported ways to select the microSD profile."""
        result = run_process(
            [str(ROOT / "fplinux"), command, "--help"],
            name=f"fplinux {command} help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )
        assert (result.returncode) == (0), result.stderr
        assert ("--boot {microsd}") in (result.stdout)
        assert ("--profile NAME") in (result.stdout)

    def test_nand_backup_help_lists_the_target_and_profile_selector(self) -> None:
        """NAND backup help offers the target choice and the documented profile option."""
        result = run_process(
            [str(ROOT / "fplinux"), "nand", "backup", "--help"],
            name="fplinux nand backup help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("nokia-ta1618") in (result.stdout)
        assert ("--profile NAME") in (result.stdout)

    def test_device_data_help_exposes_every_preparation_input(self) -> None:
        """The canonical command exposes the complete fitted-data source contract."""
        result = run_process(
            [str(ROOT / "fplinux"), "device-data", "prepare", "--help"],
            name="fplinux device-data prepare help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )

        assert (result.returncode) == (0), result.stderr
        assert ("nokia-ta1618") in (result.stdout)
        assert ("--from-dump PATH") in (result.stdout)
        assert ("--jobs N") in (result.stdout)
        assert ("--offline") in (result.stdout)
