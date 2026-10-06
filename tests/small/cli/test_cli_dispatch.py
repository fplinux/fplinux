# SPDX-License-Identifier: GPL-2.0-only
"""In-process CLI dispatcher selection of the one cache lock."""

from __future__ import annotations

import argparse
import contextlib
import io
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import __main__ as cli
from fplinux_cli.cli import dispatch as cli_dispatch
from fplinux_cli.cli import lifecycle

if TYPE_CHECKING:
    from collections.abc import Iterator


class CliCacheLockTests:
    """Keep lock-mode selection at the in-process command dispatcher."""

    @pytest.fixture(autouse=True)
    def dispatch_checkout(self) -> Iterator[None]:
        """Provide a disposable source root for each dispatch test."""
        with tempfile.TemporaryDirectory() as temporary:
            self.root = Path(temporary) / "source"
            self.root.mkdir()

            yield

    def _run(
        self,
        arguments: list[str],
        callback_name: str,
    ) -> tuple[list[object], mock.Mock]:
        """Run one command with a fake cache lock that records when it is held and released."""
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
            events.append("unlock")

        callback = mock.Mock(side_effect=lambda *_args, **_kwargs: events.append("command"))
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(
                cli,
                "discover_targets",
                return_value=("target", "nokia-ta1618"),
            ),
            mock.patch.object(lifecycle, "cache_lock", side_effect=record_lock),
            mock.patch.object(cli_dispatch, callback_name, callback),
        ):
            cli.main()
        return events, callback

    @pytest.mark.parametrize(
        ("arguments", "callback_name", "exclusive", "target", "profile"),
        [
            (["build", "target", "--jobs", "1"], "build", True, "target", None),
            (["check"], "check", True, None, None),
            (["test", "tests.small.environment.test_common"], "run_tests", True, None, None),
            (["inspect", "bundle", "target"], "inspect_bundle", False, "target", None),
            (
                ["inspect", "footprint", "target"],
                "inspect_target_footprint",
                False,
                "target",
                None,
            ),
            (["checksum", "demo-aport"], "checksum_aport", True, None, None),
            (["format", "scripts/demo.py"], "format_sources", True, None, None),
            (
                ["probe-build", "probe.c", "--output", ".cache/tools/probe"],
                "build_probe",
                True,
                None,
                None,
            ),
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
                ["nand", "identify", "nokia-ta1618"],
                "identify_target_nand",
                True,
                "nokia-ta1618",
                None,
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
        ],
        ids=[
            "build-target---jobs-1",
            "check",
            "test-tests.small.environment.test_common",
            "inspect-bundle-target",
            "inspect-footprint-target",
            "checksum-demo-aport",
            "format-scripts/demo.py",
            "probe-build-probe.c---output-.cache/tools/probe",
            "setup",
            "prune---apply",
            "package-target",
            "run-target",
            "verify-target",
            "console-target",
            "nand-backup-nokia-ta1618-backup.bin",
            "nand-backup-nokia-ta1618-backup.bin---profile-microsd-uboot",
            "nand-identify-nokia-ta1618",
            "device-data-prepare-nokia-ta1618---from-dump-saved-nand.bin---jobs-2---offline",
        ],
    )
    def test_dispatcher_chooses_the_required_lock_mode(
        self,
        *,
        arguments: list[str],
        callback_name: str,
        exclusive: bool,
        target: str | None,
        profile: str | None,
    ) -> None:
        """Build-side commands request exclusive mode; consumers request shared mode."""
        events, callback = self._run(arguments, callback_name)
        assert (events) == (
            [
                ("lock", self.root / ".cache", exclusive, arguments[0], target, profile),
                "command",
                "unlock",
            ]
        )
        if callback_name == "backup_target_nand":
            assert (callback.call_args) == (
                mock.call(
                    "nokia-ta1618",
                    Path("backup.bin"),
                    profile=profile,
                    build_type="release",
                )
            )
        elif callback_name == "identify_target_nand":
            assert (callback.call_args) == (
                mock.call("nokia-ta1618", profile=None, build_type="release")
            )
        elif callback_name == "prepare_device_data":
            assert (callback.call_args) == (
                mock.call(
                    "nokia-ta1618",
                    from_dump=Path("saved-nand.bin"),
                    events=None,
                    jobs=2,
                    offline=True,
                )
            )

    def test_format_forwards_only_the_explicit_paths(self) -> None:
        """Pass the ordered source selection through the exclusive command boundary."""
        _events, formatter = self._run(
            ["format", "scripts/tool.py", "README.md"],
            "format_sources",
        )

        formatter.assert_called_once_with(["scripts/tool.py", "README.md"])

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["run", "target"], "run_target"),
            (["device-data", "prepare", "target"], "prepare_device_data"),
        ],
        ids=["run-target", "device-data-prepare-target"],
    )
    def test_loader_events_path_reaches_each_live_loader_command(
        self, arguments: list[str], callback_name: str
    ) -> None:
        """The output destination reaches the loader boundary as a filesystem path."""
        _events, callback = self._run(
            [*arguments, "--events", "loader-events.jsonl"], callback_name
        )
        assert (callback.call_args.kwargs["events"]) == (Path("loader-events.jsonl"))

    def test_build_forwards_offline_to_the_dispatcher(self) -> None:
        """The parsed build switch reaches its callback without changing lock mode."""
        events, build = self._run(["build", "target", "--offline"], "build")

        assert (events) == (
            [
                ("lock", self.root / ".cache", True, "build", "target", None),
                "command",
                "unlock",
            ]
        )
        assert build.call_args.kwargs["offline"]
        assert not (build.call_args.kwargs["verbose"])

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["build", "target"], "build"),
            (["check", "kernel"], "check"),
            (["inspect", "bundle", "target"], "inspect_bundle"),
            (["package", "target"], "package_target"),
            (["run", "target"], "run_target"),
            (["console", "target"], "console_target"),
            (["verify", "target"], "verify_booted"),
            (["nand", "identify", "target"], "identify_target_nand"),
            (["nand", "backup", "target", "nand.bin"], "backup_target_nand"),
        ],
        ids=[
            "build-target",
            "check-kernel",
            "inspect-bundle-target",
            "package-target",
            "run-target",
            "console-target",
            "verify-target",
            "nand-identify-target",
            "nand-backup-target-nand.bin",
        ],
    )
    @pytest.mark.parametrize("build_type", ["release", "debug"], ids=("release", "debug"))
    def test_build_type_defaults_and_explicit_selection_reach_every_consumer(
        self, arguments: list[str], callback_name: str, build_type: str
    ) -> None:
        """The same enum is independent of a boot profile at every selected-bundle boundary."""
        explicit = [
            *arguments,
            "--profile",
            "microsd-uboot",
            "--build-type",
            build_type,
        ]
        _events, callback = self._run(explicit, callback_name)
        assert (callback.call_args.kwargs["build_type"]) == (build_type)
        assert (callback.call_args.kwargs["profile"]) == ("microsd-uboot")
        _events, callback = self._run(arguments, callback_name)
        assert (callback.call_args.kwargs["build_type"]) == ("release")

    def test_unknown_build_type_is_rejected_before_locking(self) -> None:
        """An unsupported kernel type cannot reach build or device actions."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", "run", "target", "--build-type", "other"]),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle, "cache_lock", side_effect=AssertionError("invalid type must not lock")
            ),
            contextlib.redirect_stderr(io.StringIO()),
            pytest.raises(SystemExit) as stopped,
        ):
            cli.main()
        assert (stopped.value.code) == (2)

    @pytest.mark.parametrize(
        ("extra", "platform", "compatible"),
        [
            ([], None, None),
            (["--platform", "ums9117"], "ums9117", None),
            (["--compatible", "hammer,lte"], None, "hammer,lte"),
        ],
        ids=["defaults", "--platform-ums9117", "--compatible-hammer,lte"],
    )
    def test_target_new_forwards_the_requested_name_platform_and_identity(
        self, extra: list[str], platform: str | None, compatible: str | None
    ) -> None:
        """The new-target command passes the platform and identity options to target creation."""
        callback = mock.Mock()
        arguments = ["target", "new", "hammer-lte", "--brand", "HAMMER", "--product", "LTE"]
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments, *extra]),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("nokia-ta1618",)),
            mock.patch.object(cli_dispatch, "create_target", callback),
        ):
            cli.main()

        callback.assert_called_once_with(
            "hammer-lte",
            platform=platform,
            brand="HAMMER",
            product="LTE",
            compatible=compatible,
        )

    def test_device_data_prepare_rejects_a_nonpositive_job_limit_before_locking(self) -> None:
        """Invalid worker limits cannot create cache state or start preparation."""
        with (
            mock.patch.object(
                sys,
                "argv",
                ["fplinux", "device-data", "prepare", "nokia-ta1618", "--jobs", "0"],
            ),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("nokia-ta1618",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("invalid arguments must not lock"),
            ) as lock,
            contextlib.redirect_stderr(io.StringIO()),
            pytest.raises(SystemExit) as stopped,
        ):
            cli.main()

        assert (stopped.value.code) == (2)
        lock.assert_not_called()
        assert not ((self.root / ".cache").exists())

    def test_check_forwards_the_kernel_worker_limit_without_changing_its_lock(self) -> None:
        """Pass the requested kernel limit through the existing exclusive check boundary."""
        events, check = self._run(["check", "kernel", "--jobs", "2"], "check")

        assert (events) == (
            [
                ("lock", self.root / ".cache", True, "check", None, None),
                "command",
                "unlock",
            ]
        )
        assert (check.call_args) == (
            mock.call(
                ["kernel"],
                profile=None,
                build_type="release",
                verbose=False,
                no_cache=False,
                jobs=2,
            )
        )

    @pytest.mark.parametrize(
        ("arguments", "scopes", "verbose", "jobs"),
        [
            (["check"], [], False, 3),
            (["check", "kernel"], ["kernel"], False, 3),
            (["check", "docs"], ["docs"], False, 1),
            (["check", "--verbose"], [], True, 1),
        ],
        ids=["check", "check-kernel", "check-docs", "check---verbose"],
    )
    def test_check_defaults_match_the_selected_execution_boundary(
        self, *, arguments: list[str], scopes: list[str], verbose: bool, jobs: int
    ) -> None:
        """Use three kernel workers normally and one when no parallel work can run."""
        _events, check = self._run(arguments, "check")
        check.assert_called_once_with(
            scopes,
            profile=None,
            build_type="release",
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

        assert (events) == (
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
                "unlock",
            ]
        )
        assert (build.call_args.kwargs["profile"]) == ("microsd-uboot")

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["package", "target", "--profile", "microsd-uboot", "--candidate"], "package_target"),
            (["run", "target", "--profile", "microsd-uboot"], "run_target"),
            (
                ["console", "target", "--profile", "microsd-uboot", "--exec", "id"],
                "console_target",
            ),
        ],
        ids=[
            "package-target---profile-microsd-uboot---candidate",
            "run-target---profile-microsd-uboot",
            "console-target---profile-microsd-uboot---exec-id",
        ],
    )
    def test_profile_consumers_use_the_selected_shared_lock_identity(
        self, arguments: list[str], callback_name: str
    ) -> None:
        """Package, run and console retain the named bundle slot under the shared cache lock."""
        events, callback = self._run(arguments, callback_name)

        assert (events) == (
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
                "unlock",
            ]
        )
        assert (callback.call_args.kwargs["profile"]) == ("microsd-uboot")

    @pytest.mark.parametrize(
        ("arguments", "callback_name", "expected_call"),
        [
            (
                ["run", "nokia-ta1618", "--boot", "microsd"],
                "run_target",
                mock.call(
                    "nokia-ta1618", profile=None, boot="microsd", build_type="release", events=None
                ),
            ),
            (
                ["package", "nokia-ta1618", "--boot", "microsd", "--candidate"],
                "package_target",
                mock.call(
                    "nokia-ta1618",
                    profile=None,
                    boot="microsd",
                    candidate=True,
                    build_type="release",
                ),
            ),
            (
                ["run", "target", "--boot", "microsd"],
                "run_target",
                mock.call(
                    "target", profile=None, boot="microsd", build_type="release", events=None
                ),
            ),
        ],
        ids=[
            "run-nokia-ta1618---boot-microsd",
            "package-nokia-ta1618---boot-microsd---candidate",
            "run-target---boot-microsd",
        ],
    )
    def test_microsd_boot_selector_locks_the_single_selected_context(
        self, arguments: list[str], callback_name: str, expected_call: object
    ) -> None:
        """The boot selector keeps its target and chooses one profile cache slot."""
        command, target = arguments[:2]
        events, callback = self._run(arguments, callback_name)

        assert (events) == (
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
                "unlock",
            ]
        )
        assert (callback.call_args) == (expected_call)

    @pytest.mark.parametrize("command", ["run", "package"], ids=["run", "package"])
    def test_boot_selector_and_profile_are_rejected_together_before_locking(
        self, command: str
    ) -> None:
        """One run or package invocation selects either a boot mode or a profile, not both."""
        with (
            mock.patch.object(
                sys,
                "argv",
                ["fplinux", command, "target", "--boot", "microsd", "--profile", "microsd-uboot"],
            ),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("conflicting selectors must not lock"),
            ) as lock,
            contextlib.redirect_stderr(io.StringIO()),
            pytest.raises(SystemExit) as stopped,
        ):
            cli.main()

        assert (stopped.value.code) == (2)
        lock.assert_not_called()
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        ("command", "callback"),
        [("build", "build"), ("console", "console_target"), ("verify", "verify_booted")],
        ids=["build", "console", "verify"],
    )
    def test_explicit_default_reuses_the_implicit_context(
        self, command: str, callback: str
    ) -> None:
        """The alias reaches each consumer with the same profile and lock identity."""
        implicit_events, implicit = self._run([command, "target"], callback)
        explicit_events, explicit = self._run(
            [command, "target", "--profile", "default"], callback
        )
        assert (explicit_events) == (implicit_events)
        assert (explicit.call_args) == (implicit.call_args)

    def test_named_profile_keeps_explicit_check_scopes(self) -> None:
        """Both boot modes can run the same source checks without selecting Kbuild."""
        _events, check = self._run(
            ["check", "python", "source", "--profile", "microsd-uboot"], "check"
        )
        check.assert_called_once_with(
            ["python", "source"],
            profile="microsd-uboot",
            build_type="release",
            verbose=False,
            no_cache=False,
            jobs=1,
        )

    def test_verify_forwards_the_selected_profile(self) -> None:
        """A microSD verification cannot silently resolve the default bundle."""
        events, verify = self._run(
            ["verify", "target", "--profile", "microsd-uboot"], "verify_booted"
        )
        assert (events) == (
            [
                ("lock", self.root / ".cache", False, "verify", "target", "microsd-uboot"),
                "command",
                "unlock",
            ]
        )
        assert (verify.call_args) == (
            mock.call("target", profile="microsd-uboot", build_type="release")
        )

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (
                ("check", "--profile", "../../x"),
                "argument --profile: invalid profile name: '../../x'",
            ),
            (
                ("build", "target", "--profile", "../../x"),
                "argument --profile: invalid profile name: '../../x'",
            ),
            (
                ("package", "target", "--profile", "../../x"),
                "argument --profile: invalid profile name: '../../x'",
            ),
            (
                ("run", "target", "--profile", "../../x"),
                "argument --profile: invalid profile name: '../../x'",
            ),
            (
                ("console", "target", "--profile", "../../x"),
                "argument --profile: invalid profile name: '../../x'",
            ),
            (
                ("check", "source", "--profile", "lab"),
                "argument --profile: unknown profile: 'lab'",
            ),
        ],
        ids=[
            "check---profile-../../x",
            "build-target---profile-../../x",
            "package-target---profile-../../x",
            "run-target---profile-../../x",
            "console-target---profile-../../x",
            "check-source---profile-lab",
        ],
    )
    def test_invalid_profile_is_rejected_before_any_cache_or_retention_action(
        self, arguments: tuple[str, ...], message: str
    ) -> None:
        """Only a public profile name reaches the cache lock or deferred cleanup."""
        stderr = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("invalid profile must not take the cache lock"),
            ) as lock,
            mock.patch.object(
                lifecycle,
                "discard_obsolete_rootfs",
                side_effect=AssertionError("invalid profile must not prune rootfs"),
            ) as rootfs_gc,
            mock.patch.object(
                lifecycle,
                "discard_superseded_profile_logs",
                side_effect=AssertionError("invalid profile must not prune logs"),
            ) as logs_gc,
            contextlib.redirect_stderr(stderr),
            pytest.raises(SystemExit, match="2"),
        ):
            cli.main()
        assert (message) in (stderr.getvalue())
        lock.assert_not_called()
        rootfs_gc.assert_not_called()
        logs_gc.assert_not_called()
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        ("command", "target", "expected_build_cleanup"),
        [("build", "nokia", True), ("check", None, False)],
        ids=["build", "check"],
    )
    def test_failed_profile_commands_run_selected_cache_cleanup_in_finally(
        self, *, command: str, target: str | None, expected_build_cleanup: bool
    ) -> None:
        """A failed action still reaches the cleanup boundary with its selected context."""
        profile = "microsd-uboot"
        cache = self.root / ".cache"
        arguments = argparse.Namespace(
            command=command,
            target=target,
            profile=profile,
            list_scopes=False,
        )
        action = mock.Mock(side_effect=SystemExit("forced profile failure"))
        with (
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                return_value=contextlib.nullcontext(),
            ),
            mock.patch.object(lifecycle, "discard_obsolete_rootfs") as rootfs_cleanup,
            mock.patch.object(lifecycle, "discard_obsolete_apks") as apks_cleanup,
            mock.patch.object(
                lifecycle,
                "discard_superseded_profile_logs",
            ) as log_cleanup,
            pytest.raises(SystemExit, match="forced profile failure"),
        ):
            lifecycle.execute_command(
                arguments,
                action,
            )

        expected_calls = [mock.call(cache)] if expected_build_cleanup else []
        assert (rootfs_cleanup.call_args_list) == (expected_calls)
        assert (apks_cleanup.call_args_list) == (expected_calls)
        assert (log_cleanup.call_args) == (
            mock.call(cache, command, profile=profile, target=target)
        )

    @pytest.mark.parametrize(
        ("arguments", "prune_arguments"),
        [(["check", "--list"], None), (["prune"], False)],
        ids=["check---list", "prune"],
    )
    def test_check_list_and_dry_prune_dispatch_without_the_cache_lock(
        self, *, arguments: list[str], prune_arguments: bool | None
    ) -> None:
        """The dispatcher neither locks nor creates the cache for the two no-work commands.

        `check --list` runs for real. `prune` is replaced at the dispatcher, so this
        checks only that the dry-run choice reaches it; prune's own read-only behavior
        is not exercised here.
        """
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(lifecycle, "ROOT", self.root),
            mock.patch.object(cli, "discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("this path must not lock"),
            ) as lock,
            mock.patch.object(cli_dispatch, "prune") as prune,
            mock.patch("builtins.print"),
        ):
            cli.main()
        lock.assert_not_called()
        if prune_arguments is not None:
            prune.assert_called_once_with(apply=prune_arguments)
        assert not ((self.root / ".cache").exists())
