# SPDX-License-Identifier: GPL-2.0-only
"""Execute staged Python consumers against their selected source snapshots."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.bootstrap import recipe as bootstrap_build
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.workspace import build_inputs as workspace_module
from fplinux_cli.workspace import capture as workspace_capture
from fplinux_cli.workspace import staging as workspace_staging


class StagedWorkspaceTests:
    """Load staged build imports and compute recipes in real Python processes."""

    def test_microsd_source_snapshot_supports_build_imports_and_both_configurations(self) -> None:
        """The staged source loads the build consumer and selected/default boot policies."""
        sources = workspace_module.target_build_source_files("nokia-ta1618", "microsd-uboot")
        snapshot = workspace_capture.workspace_snapshot(sources)
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(workspace_staging, "ROOT", Path(temporary)):
                staged = workspace_staging.stage_workspace_snapshot(snapshot)
            imported = subprocess.run(
                [sys.executable, "-B", "-c", "import fplinux_cli.build.__main__"],
                cwd=staged,
                env={"PYTHONPATH": str(staged / "scripts")},
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
            assert (imported.returncode) == (0), imported.stderr
            with mock.patch.object(common, "ROOT", staged):
                card = targets.load_target("nokia-ta1618", "microsd-uboot")
                ram = targets.load_target("nokia-ta1618")

            assert (card["linux"]["root"]["kind"]) == ("external")
            assert (ram["linux"]["root"]) == ({"kind": "initramfs"})

    def test_default_source_snapshot_supports_build_import(self) -> None:
        """An ordinary RAM build loads its consumer from only staged inputs."""
        snapshot = workspace_capture.workspace_snapshot(
            workspace_module.target_build_source_files("nokia-ta1618")
        )
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(workspace_staging, "ROOT", Path(temporary)):
                staged = workspace_staging.stage_workspace_snapshot(snapshot)
            imported = subprocess.run(
                [sys.executable, "-B", "-c", "import fplinux_cli.build.__main__"],
                cwd=staged,
                env={"PYTHONPATH": str(staged / "scripts")},
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
        assert (imported.returncode) == (0), imported.stderr

    @pytest.mark.parametrize("profile", [None, "microsd-uboot"], ids=["ram", "microsd"])
    def test_staged_bootstrap_reads_panel_and_font_inputs_for_ram_and_sd(
        self, profile: str | None
    ) -> None:
        """The staged consumer reads its full recipe and reacts to its font interface."""
        sources = common.load_toml(common.ROOT / "sources.lock.toml")
        target = targets.load_target("nokia-ta1618", profile)
        platform = load_platform(target["platform"])
        expected = bootstrap_build.bootstrap_recipe_digest(
            sources, "nokia-ta1618", target, platform
        )
        snapshot = workspace_capture.workspace_snapshot(
            workspace_module.target_build_source_files("nokia-ta1618", profile)
        )
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(workspace_staging, "ROOT", Path(temporary)):
                staged = workspace_staging.stage_workspace_snapshot(snapshot)
            command = (
                "import sys; from fplinux_cli import common; "
                "from fplinux_cli.build.bootstrap.recipe import bootstrap_recipe_digest; "
                "from fplinux_cli.manifests.targets import load_target; "
                "from fplinux_cli.manifests.platforms import load_platform; "
                "target = load_target('nokia-ta1618', sys.argv[1] or None); "
                "platform = load_platform(target['platform']); "
                "sources = common.load_toml(common.ROOT / 'sources.lock.toml'); "
                "print(bootstrap_recipe_digest(sources, 'nokia-ta1618', target, platform))"
            )

            def staged_digest(command: str, profile: str | None, staged: Path) -> str:
                result = subprocess.run(
                    [sys.executable, "-B", "-c", command, profile or ""],
                    cwd=staged,
                    env={"PYTHONPATH": str(staged / "scripts")},
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=20,
                )
                assert (result.returncode) == (0), result.stdout + result.stderr
                return result.stdout.strip()

            assert (staged_digest(command, profile, staged)) == (expected)
            (staged / "unrelated.c").write_bytes(b"int unrelated;\n")
            assert (staged_digest(command, profile, staged)) == (expected)
            header = staged / "bootstrap/fplinux-boot-screen/font.h"
            header.write_bytes(header.read_bytes() + b"\n/* Changed compile interface. */\n")
            assert (staged_digest(command, profile, staged)) != (expected)
