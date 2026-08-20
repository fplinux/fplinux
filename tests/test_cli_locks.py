# SPDX-License-Identifier: GPL-2.0-only
"""Public CLI selection of the one cache lock."""

from __future__ import annotations

import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import __main__ as cli

if TYPE_CHECKING:
    from collections.abc import Iterator


class CliCacheLockTests(unittest.TestCase):
    """Keep locking at the repository CLI boundary."""

    def setUp(self) -> None:
        """Provide a disposable source root for each dispatch test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()

    def tearDown(self) -> None:
        """Discard the disposable source root."""
        self.temporary.cleanup()

    def _run(
        self,
        arguments: list[str],
        callback_name: str,
    ) -> tuple[list[object], mock.Mock]:
        """Run one command with a context manager that records public dispatch order."""
        events: list[object] = []

        @contextmanager
        def record_lock(
            cache_root: Path,
            *,
            exclusive: bool,
            command: str,
            target: str | None,
        ) -> Iterator[None]:
            events.append(("lock", cache_root, exclusive, command, target))
            try:
                yield
            finally:
                events.append("unlock")

        callback = mock.Mock(side_effect=lambda *_args, **_kwargs: events.append("command"))
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(cli, "cache_lock", side_effect=record_lock),
            mock.patch.object(cli, callback_name, callback),
        ):
            cli.main()
        return events, callback

    def test_public_commands_choose_the_required_lock_mode(self) -> None:
        """Build-side commands are exclusive; consumers take the shared flock."""
        cases = (
            (["build", "target", "--jobs", "1"], "build", True, "target"),
            (["check"], "check", True, None),
            (["setup"], "setup", True, None),
            (["prune", "--apply"], "prune", True, None),
            (["package", "target"], "package_target", False, "target"),
            (["run", "target"], "run_target", False, "target"),
            (["verify", "target"], "verify_booted", False, "target"),
            (["console", "target"], "console_target", False, "target"),
        )
        for arguments, callback_name, exclusive, target in cases:
            with self.subTest(arguments=arguments):
                events, callback = self._run(arguments, callback_name)
                self.assertEqual(
                    events,
                    [
                        ("lock", self.root / ".cache", exclusive, arguments[0], target),
                        "command",
                        "unlock",
                    ],
                )
                callback.assert_called_once()

    def test_lock_is_released_when_the_command_raises(self) -> None:
        """The CLI does not retain the flock when a command fails."""
        events: list[object] = []

        @contextmanager
        def record_lock(*_args: object, **_kwargs: object) -> Iterator[None]:
            events.append("lock")
            try:
                yield
            finally:
                events.append("unlock")

        with (
            mock.patch.object(sys, "argv", ["fplinux", "build", "target", "--jobs", "1"]),
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(cli, "cache_lock", side_effect=record_lock),
            mock.patch.object(cli, "build", side_effect=RuntimeError("expected failure")),
            self.assertRaisesRegex(RuntimeError, "expected failure"),
        ):
            cli.main()
        self.assertEqual(events, ["lock", "unlock"])

    def test_build_forwards_offline_to_the_dispatcher(self) -> None:
        """The public build switch reaches build without changing its cache lock."""
        events, build = self._run(["build", "target", "--offline"], "build")

        self.assertEqual(
            events,
            [
                ("lock", self.root / ".cache", True, "build", "target"),
                "command",
                "unlock",
            ],
        )
        self.assertTrue(build.call_args.kwargs["offline"])
        self.assertFalse(build.call_args.kwargs["verbose"])

    def test_check_list_and_dry_prune_do_not_touch_cache(self) -> None:
        """The two no-work paths neither lock nor create a cache directory."""
        cases = (
            (["check", "--list"], None),
            (["prune"], (False, False)),
            (["prune", "--json"], (True, False)),
        )
        for arguments, prune_arguments in cases:
            with self.subTest(arguments=arguments):
                with (
                    mock.patch.object(sys, "argv", ["fplinux", *arguments]),
                    mock.patch.object(cli, "ROOT", self.root),
                    mock.patch.object(cli, "discover_targets", return_value=("target",)),
                    mock.patch.object(
                        cli,
                        "cache_lock",
                        side_effect=AssertionError("this path must not lock"),
                    ) as lock,
                    mock.patch.object(cli, "prune") as prune,
                    mock.patch("builtins.print"),
                ):
                    cli.main()
                lock.assert_not_called()
                if prune_arguments is not None:
                    json_output, apply = prune_arguments
                    prune.assert_called_once_with(json_output=json_output, apply=apply)
                self.assertFalse((self.root / ".cache").exists())


if __name__ == "__main__":
    unittest.main()
