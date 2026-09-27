# SPDX-License-Identifier: GPL-2.0-only
"""Parser behavior for automatic and explicit build parallelism."""

from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

from fplinux_cli import __main__ as cli


class BuildParallelismTests(unittest.TestCase):
    """Automatic defaults respect available CPUs; explicit limits remain unchanged."""

    def selected_jobs(self, command: str, cpus: int | None, *options: str) -> int:
        """Observe dispatch while replacing external build and device operations."""
        arguments = (
            ["build", "demo", *options]
            if command == "build"
            else ["device-data", "prepare", "demo", *options]
        )
        with (
            mock.patch.object(cli, "discover_targets", return_value=("demo",)),
            mock.patch.object(os, "process_cpu_count", return_value=cpus),
            mock.patch.object(cli, "build") as build,
            mock.patch.object(cli, "prepare_device_data") as prepare,
            mock.patch.object(
                cli, "_dispatch_with_cache_lock", side_effect=lambda _args, action: action()
            ),
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
        ):
            cli.main()
        value = build.call_args.args[1] if command == "build" else prepare.call_args.kwargs["jobs"]
        self.assertIsInstance(value, int)
        return int(value)

    def test_default_worker_limit_uses_available_cpus_up_to_eight(self) -> None:
        """Neither build path oversubscribes affinity or automatically uses all 12 CPUs."""
        for command in ("build", "device-data"):
            for cpus, expected in ((None, 1), (1, 1), (4, 4), (12, 8)):
                with self.subTest(command=command, cpus=cpus):
                    self.assertEqual(self.selected_jobs(command, cpus), expected)

    def test_explicit_worker_limit_is_not_replaced_by_the_default(self) -> None:
        """An explicitly chosen limit keeps its existing CLI meaning."""
        for command in ("build", "device-data"):
            with self.subTest(command=command):
                self.assertEqual(self.selected_jobs(command, 12, "--jobs", "3"), 3)


if __name__ == "__main__":
    unittest.main()
