# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for setup's reporting and preparation boundaries."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.environment import kern
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.reporting import run as reporting

if TYPE_CHECKING:
    from pathlib import Path


def test_ready_direct_setup_finishes_its_own_reporter(tmp_path: Path) -> None:
    """A prepared-environment hit publishes successful public setup run metadata."""
    expected = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    with (
        mock.patch.object(env_setup, "ROOT", tmp_path),
        mock.patch.object(kern, "ROOT", tmp_path),
        mock.patch.object(reporting, "ROOT", tmp_path),
        mock.patch.object(env_setup, "install_kern", return_value="kern"),
        mock.patch.object(env_setup, "environment_recipe_digest", return_value="a" * 64),
        mock.patch.object(env_setup, "current_environment_state", return_value=expected),
        mock.patch.object(env_setup, "install_git_hooks"),
    ):
        assert env_setup.setup(lock={}) == expected
    paths = list((tmp_path / ".cache/logs/setup").glob("*/run.json"))
    assert len(paths) == 1
    assert json.loads(paths[0].read_text())["status"] == "success"


def test_tool_installation_reserves_eight_gib_in_the_kern_command(tmp_path: Path) -> None:
    """The complete tool installation retains its explicit preparation memory budget."""
    reporter = mock.MagicMock()
    lock = {
        "input": [
            {
                "purpose": "environment-base",
                "destination": "downloads/base.tar.gz",
            },
            {
                "purpose": "environment-package",
                "destination": "downloads/tool.apk",
            },
        ]
    }
    with mock.patch.object(env_setup, "kern_environment", return_value={}):
        env_setup._prepare_rootfs(  # noqa: SLF001 -- inspect the installation execution boundary.
            "kern",
            tmp_path / "candidate",
            context=tmp_path / "recipe",
            lock=lock,
            recipe="a" * 64,
            generation="b" * 64,
            reporter=reporter,
        )
    commands = [
        call.args[0]
        for call in reporter.stage.return_value.__enter__.return_value.run.call_args_list
    ]
    installation = next(command for command in commands if command[:2] == ["kern", "box"])
    assert installation[installation.index("--memory") + 1] == "8192m"
