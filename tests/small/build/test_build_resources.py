# SPDX-License-Identifier: GPL-2.0-only
"""Parser behavior for automatic and explicit build parallelism."""

from __future__ import annotations

import os
import sys
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import __main__ as cli

if TYPE_CHECKING:
    import argparse
    from collections.abc import Callable
    from pathlib import Path


class BuildParallelismTests:
    """Automatic defaults respect available CPUs; explicit limits remain unchanged."""

    @staticmethod
    def selected_jobs(root: Path, command: str, cpus: int | None, *options: str) -> int:
        """Observe dispatch while replacing external build and device operations."""
        arguments = (
            ["build", "demo", *options]
            if command == "build"
            else ["device-data", "prepare", "demo", *options]
        )
        selected_root = root / ".data"

        def execute_command(
            _args: argparse.Namespace, action: Callable[[], None], *, data_directory: Path
        ) -> None:
            assert data_directory == selected_root
            action()

        with (
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("demo",)),
            mock.patch.object(os, "process_cpu_count", return_value=cpus),
            mock.patch.dict(os.environ, {"FPLINUX_DATA_DIR": str(selected_root)}),
            mock.patch("fplinux_cli.build.workflow.build") as build,
            mock.patch("fplinux_cli.cli.device_data.prepare_device_data") as prepare,
            mock.patch("fplinux_cli.cli.lifecycle.execute_command", side_effect=execute_command),
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
        ):
            cli.main()
        value = build.call_args.args[1] if command == "build" else prepare.call_args.kwargs["jobs"]
        assert isinstance(value, int)
        return int(value)

    @pytest.mark.parametrize("command", ["build", "device-data"])
    @pytest.mark.parametrize(
        ("cpus", "expected"),
        [
            pytest.param(None, 1, id="unknown-cpus"),
            pytest.param(1, 1, id="one-cpu"),
            pytest.param(4, 4, id="four-cpus"),
            pytest.param(12, 8, id="twelve-cpus-capped-at-eight"),
        ],
    )
    def test_default_worker_limit_uses_available_cpus_up_to_eight(
        self, tmp_path: Path, command: str, cpus: int | None, expected: int
    ) -> None:
        """Neither build path oversubscribes affinity or automatically uses all 12 CPUs."""
        assert self.selected_jobs(tmp_path, command, cpus) == expected

    @pytest.mark.parametrize("command", ["build", "device-data"])
    def test_explicit_worker_limit_is_not_replaced_by_the_default(
        self, tmp_path: Path, command: str
    ) -> None:
        """An explicitly chosen limit keeps its existing CLI meaning."""
        assert self.selected_jobs(tmp_path, command, 12, "--jobs", "3") == 3
