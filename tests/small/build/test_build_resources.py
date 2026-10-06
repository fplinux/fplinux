# SPDX-License-Identifier: GPL-2.0-only
"""Parser behavior for automatic and explicit build parallelism."""

from __future__ import annotations

import os
import sys
from unittest import mock

import pytest
from fplinux_cli import __main__ as cli
from fplinux_cli.cli import dispatch as cli_dispatch


class BuildParallelismTests:
    """Automatic defaults respect available CPUs; explicit limits remain unchanged."""

    @staticmethod
    def selected_jobs(command: str, cpus: int | None, *options: str) -> int:
        """Observe dispatch while replacing external build and device operations."""
        arguments = (
            ["build", "demo", *options]
            if command == "build"
            else ["device-data", "prepare", "demo", *options]
        )
        with (
            mock.patch.object(cli, "discover_targets", return_value=("demo",)),
            mock.patch.object(os, "process_cpu_count", return_value=cpus),
            mock.patch.object(cli_dispatch, "build") as build,
            mock.patch.object(cli_dispatch, "prepare_device_data") as prepare,
            mock.patch.object(cli, "execute_command", side_effect=lambda _args, action: action()),
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
        self, command: str, cpus: int | None, expected: int
    ) -> None:
        """Neither build path oversubscribes affinity or automatically uses all 12 CPUs."""
        assert self.selected_jobs(command, cpus) == expected

    @pytest.mark.parametrize("command", ["build", "device-data"])
    def test_explicit_worker_limit_is_not_replaced_by_the_default(self, command: str) -> None:
        """An explicitly chosen limit keeps its existing CLI meaning."""
        assert self.selected_jobs(command, 12, "--jobs", "3") == 3
