# SPDX-License-Identifier: GPL-2.0-only
"""In-process CLI dispatcher selection of the one cache lock."""

from __future__ import annotations

import argparse
import contextlib
import io
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
    """Keep lock-mode selection at the in-process command dispatcher."""

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
        """Run one command with a context manager that records dispatcher order."""
        events: list[object] = []

        @contextmanager
        def record_lock(
            cache_root: Path,
            *,
            exclusive: bool,
            command: str,
            target: str | None,
            profile: str | None = None,
        ) -> Iterator[None]:
            events.append(("lock", cache_root, exclusive, command, target, profile))
            yield

        callback = mock.Mock(side_effect=lambda *_args, **_kwargs: events.append("command"))
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(
                cli,
                "discover_targets",
                return_value=("target", "nokia-ta1618"),
            ),
            mock.patch.object(cli, "cache_lock", side_effect=record_lock),
            mock.patch.object(cli, callback_name, callback),
        ):
            cli.main()
        return events, callback

    def test_dispatcher_chooses_the_required_lock_mode(self) -> None:
        """Build-side commands request exclusive mode; consumers request shared mode."""
        cases: tuple[tuple[list[str], str, bool, str | None, str | None], ...] = (
            (["build", "target", "--jobs", "1"], "build", True, "target", None),
            (["check"], "check", True, None, None),
            (["test", "tests.small.test_common"], "run_tests", True, None, None),
            (["checksum", "demo-aport"], "checksum_aport", True, None, None),
            (["format", "scripts/demo.py"], "format_sources", True, None, None),
            (["setup"], "setup", True, None, None),
            (["prune", "--apply"], "prune", True, None, None),
            (["package", "target"], "package_target", False, "target", None),
            (["run", "target"], "run_target", False, "target", None),
            (["verify", "target"], "verify_booted", False, "target", None),
            (["console", "target"], "console_target", False, "target", None),
            (
                ["nand", "backup", "nokia-ta1618", "backup.bin"],
                "backup_target_nand",
                True,
                "nokia-ta1618",
                None,
            ),
            (
                ["nand", "backup", "nokia-ta1618", "backup.bin", "--profile", "microsd-uboot"],
                "backup_target_nand",
                True,
                "nokia-ta1618",
                "microsd-uboot",
            ),
            (
                [
                    "device-data",
                    "prepare",
                    "nokia-ta1618",
                    "--from-dump",
                    "saved-nand.bin",
                    "--jobs",
                    "2",
                    "--offline",
                ],
                "prepare_device_data",
                True,
                "nokia-ta1618",
                None,
            ),
        )
        for arguments, callback_name, exclusive, target, profile in cases:
            with self.subTest(arguments=arguments):
                events, callback = self._run(arguments, callback_name)
                self.assertEqual(
                    events,
                    [
                        ("lock", self.root / ".cache", exclusive, arguments[0], target, profile),
                        "command",
                    ],
                )
                if callback_name == "backup_target_nand":
                    self.assertEqual(
                        callback.call_args,
                        mock.call("nokia-ta1618", Path("backup.bin"), profile=profile),
                    )
                elif callback_name == "prepare_device_data":
                    self.assertEqual(
                        callback.call_args,
                        mock.call(
                            "nokia-ta1618",
                            from_dump=Path("saved-nand.bin"),
                            jobs=2,
                            offline=True,
                        ),
                    )

    def test_format_forwards_only_the_explicit_paths(self) -> None:
        """Pass the ordered source selection through the exclusive command boundary."""
        _events, formatter = self._run(
            ["format", "scripts/tool.py", "README.md"],
            "format_sources",
        )

        formatter.assert_called_once_with(["scripts/tool.py", "README.md"])

    def test_build_forwards_offline_to_the_dispatcher(self) -> None:
        """The parsed build switch reaches its callback without changing lock mode."""
        events, build = self._run(["build", "target", "--offline"], "build")

        self.assertEqual(
            events,
            [
                ("lock", self.root / ".cache", True, "build", "target", None),
                "command",
            ],
        )
        self.assertTrue(build.call_args.kwargs["offline"])
        self.assertFalse(build.call_args.kwargs["verbose"])

    def test_device_data_prepare_rejects_a_nonpositive_job_limit_before_locking(self) -> None:
        """Invalid worker limits cannot create cache state or start preparation."""
        with (
            mock.patch.object(
                sys,
                "argv",
                ["fplinux", "device-data", "prepare", "nokia-ta1618", "--jobs", "0"],
            ),
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("nokia-ta1618",)),
            mock.patch.object(
                cli,
                "cache_lock",
                side_effect=AssertionError("invalid arguments must not lock"),
            ) as lock,
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as stopped,
        ):
            cli.main()

        self.assertEqual(stopped.exception.code, 2)
        lock.assert_not_called()
        self.assertFalse((self.root / ".cache").exists())

    def test_check_forwards_the_kernel_worker_limit_without_changing_its_lock(self) -> None:
        """Pass the requested kernel limit through the existing exclusive check boundary."""
        events, check = self._run(["check", "kernel", "--jobs", "2"], "check")

        self.assertEqual(
            events,
            [
                ("lock", self.root / ".cache", True, "check", None, None),
                "command",
            ],
        )
        self.assertEqual(
            check.call_args,
            mock.call(
                ["kernel"],
                profile=None,
                verbose=False,
                no_cache=False,
                jobs=2,
            ),
        )

    def test_check_defaults_match_the_selected_execution_boundary(self) -> None:
        """Use three kernel workers normally and one when no parallel work can run."""
        cases: tuple[tuple[list[str], list[str], bool, int], ...] = (
            (["check"], [], False, 3),
            (["check", "docs"], ["docs"], False, 1),
            (["check", "--verbose"], [], True, 1),
        )
        for arguments, scopes, verbose, jobs in cases:
            with self.subTest(arguments=arguments):
                _events, check = self._run(arguments, "check")
                check.assert_called_once_with(
                    scopes,
                    profile=None,
                    verbose=verbose,
                    no_cache=False,
                    jobs=jobs,
                )

    def test_profile_is_recorded_in_the_global_cache_lock_owner(self) -> None:
        """A blocked build identifies its profile without splitting the global lock."""
        events, build = self._run(
            ["build", "target", "--profile", "microsd-uboot"],
            "build",
        )

        self.assertEqual(
            events,
            [
                (
                    "lock",
                    self.root / ".cache",
                    True,
                    "build",
                    "target",
                    "microsd-uboot",
                ),
                "command",
            ],
        )
        self.assertEqual(build.call_args.kwargs["profile"], "microsd-uboot")

    def test_profile_package_and_console_use_the_selected_shared_lock_identity(self) -> None:
        """Profile consumers retain the named bundle slot under the shared cache lock."""
        cases = (
            (
                ["package", "target", "--profile", "microsd-uboot", "--candidate"],
                "package_target",
            ),
            (
                ["console", "target", "--profile", "microsd-uboot", "--exec", "id"],
                "console_target",
            ),
        )
        for arguments, callback_name in cases:
            with self.subTest(command=arguments[0]):
                events, callback = self._run(arguments, callback_name)

                self.assertEqual(
                    events,
                    [
                        (
                            "lock",
                            self.root / ".cache",
                            False,
                            arguments[0],
                            "target",
                            "microsd-uboot",
                        ),
                        "command",
                    ],
                )
                self.assertEqual(callback.call_args.kwargs["profile"], "microsd-uboot")

    def test_microsd_boot_selector_locks_the_single_selected_context(self) -> None:
        """The boot selector keeps its target and chooses one profile cache slot."""
        cases = (
            (
                ["run", "nokia-ta1618", "--boot", "microsd"],
                "run_target",
                mock.call("nokia-ta1618", profile=None, boot="microsd"),
            ),
            (
                ["package", "nokia-ta1618", "--boot", "microsd", "--candidate"],
                "package_target",
                mock.call(
                    "nokia-ta1618",
                    profile=None,
                    boot="microsd",
                    candidate=True,
                ),
            ),
            (
                ["run", "target", "--boot", "microsd"],
                "run_target",
                mock.call("target", profile=None, boot="microsd"),
            ),
        )
        for arguments, callback_name, expected_call in cases:
            command, target = arguments[:2]
            with self.subTest(command=command):
                events, callback = self._run(arguments, callback_name)

                self.assertEqual(
                    events,
                    [
                        (
                            "lock",
                            self.root / ".cache",
                            False,
                            command,
                            target,
                            "microsd-uboot",
                        ),
                        "command",
                    ],
                )
                self.assertEqual(callback.call_args, expected_call)

    def test_explicit_default_reuses_the_implicit_context(self) -> None:
        """The alias reaches each consumer with the same profile and lock identity."""
        for command, callback in (
            ("build", "build"),
            ("console", "console_target"),
            ("verify", "verify_booted"),
        ):
            with self.subTest(command=command):
                implicit_events, implicit = self._run([command, "target"], callback)
                explicit_events, explicit = self._run(
                    [command, "target", "--profile", "default"], callback
                )
                self.assertEqual(explicit_events, implicit_events)
                self.assertEqual(explicit.call_args, implicit.call_args)

    def test_named_profile_keeps_explicit_check_scopes(self) -> None:
        """Both boot modes can run the same source checks without selecting Kbuild."""
        _events, check = self._run(
            ["check", "python", "source", "--profile", "microsd-uboot"], "check"
        )
        check.assert_called_once_with(
            ["python", "source"], profile="microsd-uboot", verbose=False, no_cache=False, jobs=1
        )

    def test_verify_forwards_the_selected_profile(self) -> None:
        """A microSD verification cannot silently resolve the default bundle."""
        events, verify = self._run(
            ["verify", "target", "--profile", "microsd-uboot"], "verify_booted"
        )
        self.assertEqual(
            events[0],
            ("lock", self.root / ".cache", False, "verify", "target", "microsd-uboot"),
        )
        self.assertEqual(
            verify.call_args,
            mock.call("target", profile="microsd-uboot"),
        )

    def test_invalid_profile_is_rejected_before_any_cache_or_retention_action(self) -> None:
        """Profile names are path components, not cache paths or deferred cleanup inputs."""
        cases = (
            ("check", "--profile", "../../x"),
            ("build", "target", "--profile", "../../x"),
            ("package", "target", "--profile", "../../x"),
            ("run", "target", "--profile", "../../x"),
            ("console", "target", "--profile", "../../x"),
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                stderr = io.StringIO()
                with (
                    mock.patch.object(sys, "argv", ["fplinux", *arguments]),
                    mock.patch.object(cli, "ROOT", self.root),
                    mock.patch.object(cli, "discover_targets", return_value=("target",)),
                    mock.patch.object(
                        cli,
                        "cache_lock",
                        side_effect=AssertionError("invalid profile must not take the cache lock"),
                    ) as lock,
                    mock.patch.object(
                        cli,
                        "discard_obsolete_rootfs",
                        side_effect=AssertionError("invalid profile must not prune rootfs"),
                    ) as rootfs_gc,
                    mock.patch.object(
                        cli,
                        "discard_superseded_profile_logs",
                        side_effect=AssertionError("invalid profile must not prune logs"),
                    ) as logs_gc,
                    contextlib.redirect_stderr(stderr),
                    self.assertRaisesRegex(SystemExit, "2"),
                ):
                    cli.main()
                self.assertIn(
                    "argument --profile: invalid profile name: '../../x'",
                    stderr.getvalue(),
                )
                lock.assert_not_called()
                rootfs_gc.assert_not_called()
                logs_gc.assert_not_called()
                self.assertFalse((self.root / ".cache").exists())

    def test_failed_profile_commands_run_selected_cache_cleanup_in_finally(self) -> None:
        """A failed action still reaches the cleanup boundary with its selected context."""
        profile = "microsd-uboot"
        cache = self.root / ".cache"
        for command, target, expected_build_cleanup in (
            ("build", "nokia", [mock.call(cache)]),
            ("check", None, []),
        ):
            with self.subTest(command=command):
                arguments = argparse.Namespace(
                    command=command,
                    target=target,
                    profile=profile,
                    list_scopes=False,
                )
                action = mock.Mock(side_effect=SystemExit("forced profile failure"))
                with (
                    mock.patch.object(cli, "ROOT", self.root),
                    mock.patch.object(
                        cli,
                        "cache_lock",
                        return_value=contextlib.nullcontext(),
                    ),
                    mock.patch.object(cli, "discard_obsolete_rootfs") as rootfs_cleanup,
                    mock.patch.object(cli, "discard_obsolete_apks") as apks_cleanup,
                    mock.patch.object(
                        cli,
                        "discard_superseded_profile_logs",
                    ) as log_cleanup,
                    self.assertRaisesRegex(SystemExit, "forced profile failure"),
                ):
                    cli._dispatch_with_cache_lock(  # noqa: SLF001 -- lock lifecycle boundary.
                        arguments,
                        action,
                    )

                self.assertEqual(rootfs_cleanup.call_args_list, expected_build_cleanup)
                self.assertEqual(apks_cleanup.call_args_list, expected_build_cleanup)
                self.assertEqual(
                    log_cleanup.call_args,
                    mock.call(cache, command, profile=profile, target=target),
                )

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
