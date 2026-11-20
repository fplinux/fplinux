# SPDX-License-Identifier: GPL-2.0-only
"""Public CLI help tests for boot selectors and diagnostic tools."""

from __future__ import annotations

import os
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
            pytest.param(("run",), id="run"),
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
        if command[0] in {"run", "console", "verify"}:
            assert "--boot {microsd}" in result.stdout
            assert "--profile NAME" in result.stdout
            assert ("--events PATH" in result.stdout) == (command[0] == "run")
        elif command == ("nand", "backup"):
            assert "nokia-ta1618" in result.stdout
            assert "--profile NAME" in result.stdout

    @pytest.mark.parametrize("command", ["run", "console", "verify"])
    def test_microsd_boot_rejects_separate_phone_data_before_creating_state(
        self, tmp_path: Path, command: str
    ) -> None:
        """A card boot always uses the phone data installed with its system."""
        data = tmp_path / "checkout store"
        result = run_process(
            [
                str(ROOT / "fplinux"),
                command,
                "nokia-ta1618",
                "--boot",
                "microsd",
                "--phone-data",
                "another-phone",
            ],
            name="system-card phone-data selection",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
            env={**os.environ, "FPLINUX_DATA_DIR": str(data)},
        )

        assert result.returncode == 2
        assert "system microSD boot uses the phone data already on the card" in result.stderr
        assert not data.exists()

    def test_package_help_exposes_distribution_selections(self) -> None:
        """Package lets callers select the phone, host and output directory."""
        result = run_process(
            [str(ROOT / "fplinux"), "package", "--help"],
            name="fplinux package help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )
        assert result.returncode == 0, result.stderr
        assert "--target" in result.stdout
        assert "konrow-senior-280-plus" in result.stdout
        assert "--host {linux,windows}" in result.stdout
        assert "--output-directory PATH" in result.stdout
        assert "--boot" not in result.stdout
        assert "--profile" not in result.stdout

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
        assert "--events PATH" in result.stdout
        assert "--offline" not in result.stdout
