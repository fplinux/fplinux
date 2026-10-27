# SPDX-License-Identifier: GPL-2.0-only
"""In-process CLI dispatcher selection of the one cache lock."""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import __main__ as cli
from fplinux_cli.cli import lifecycle

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture
def cli_environment() -> Iterator[None]:
    """Restore the environment selected by the in-process command entrypoint."""
    with mock.patch.dict(os.environ):
        yield


@pytest.fixture
def dispatch_root(monkeypatch: pytest.MonkeyPatch) -> Iterator[Path]:
    """Provide a disposable source root for each dispatch test."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / "source"
        root.mkdir()
        monkeypatch.setenv("FPLINUX_DATA_DIR", str(root / ".data"))
        yield root


def run_dispatch(
    dispatch_root: Path,
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
        mock.patch.object(lifecycle, "ROOT", dispatch_root),
        mock.patch(
            "fplinux_cli.manifests.paths.discover_targets",
            return_value=("target", "nokia-ta1618"),
        ),
        mock.patch.object(lifecycle, "cache_lock", side_effect=record_lock),
        mock.patch(callback_name, callback),
    ):
        cli.main()
    return events, callback


@pytest.mark.usefixtures("cli_environment")
class CliCacheLockTests:
    """Keep lock-mode selection at the in-process command dispatcher."""

    @pytest.mark.parametrize(
        ("arguments", "target", "host", "site", "all_inputs", "force", "offline"),
        [
            (["setup"], None, "linux", False, False, False, False),
            (["setup", "target"], "target", "linux", False, False, False, False),
            (
                ["setup", "target", "--host", "windows", "--offline", "--force"],
                "target",
                "windows",
                False,
                False,
                True,
                True,
            ),
            (["setup", "--site", "--offline"], None, "linux", True, False, False, True),
            (["setup", "--all", "--force"], None, "linux", False, True, True, False),
        ],
        ids=("environment", "target-linux", "target-windows", "site", "all"),
    )
    def test_setup_forwards_selection_under_its_exclusive_cache_lock(  # noqa: PLR0913 -- independent command expectations remain visible.
        self,
        dispatch_root: Path,
        arguments: list[str],
        target: str | None,
        host: str,
        *,
        site: bool,
        all_inputs: bool,
        force: bool,
        offline: bool,
    ) -> None:
        """The selector and default host reach preparation without creating an export path."""
        events, callback = run_dispatch(
            dispatch_root, arguments, "fplinux_cli.environment.setup.prepare_setup"
        )

        assert events == [
            ("lock", dispatch_root / ".data", True, "setup", target, None),
            "command",
            "unlock",
        ]
        assert callback.call_args == mock.call(
            target, host=host, site=site, all_inputs=all_inputs, force=force, offline=offline
        )

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (["setup", "target", "--site"], "not allowed with argument"),
            (["setup", "target", "--all"], "not allowed with argument"),
            (["setup", "--site", "--all"], "not allowed with argument"),
            (["setup", "--host", "linux"], "setup --host requires TARGET"),
            (["setup", "--site", "--host", "windows"], "setup --host requires TARGET"),
            (["setup", "--all", "--host", "windows"], "setup --host requires TARGET"),
            (["setup", "--site", "--force"], "setup --force cannot be combined with --site"),
        ],
    )
    def test_invalid_setup_selection_stops_before_cache_access(
        self, arguments: list[str], message: str, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Incompatible preparation scopes cannot acquire the writer lock or modify the cache."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle, "cache_lock", side_effect=AssertionError("unexpected cache access")
            ),
            pytest.raises(SystemExit) as error,
        ):
            cli.main()

        output = capsys.readouterr()
        assert message in f"{error.value.code}\n{output.err}"

    @pytest.mark.parametrize(
        "arguments",
        [
            ["build", "target", "--offline"],
            ["checksum", "example", "--offline"],
            ["device-data", "prepare", "target", "--offline"],
            ["device-data", "report", "target", "--from-dump", "nand.bin", "--offline"],
        ],
    )
    def test_ordinary_commands_reject_the_removed_download_policy_switch(
        self, arguments: list[str], capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Only explicit preparation commands expose a choice about downloading originals."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle, "cache_lock", side_effect=AssertionError("unexpected cache access")
            ),
            pytest.raises(SystemExit) as error,
        ):
            cli.main()

        assert error.value.code == 2
        assert "unrecognized arguments: --offline" in capsys.readouterr().err

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["build", "target"], "fplinux_cli.build.workflow.build"),
            (["run", "target"], "fplinux_cli.runtime.runner.run_target"),
            (["console", "target"], "fplinux_cli.cli.runtime.console_target"),
            (["verify", "target"], "fplinux_cli.cli.runtime.verify_booted"),
            (["nand", "identify", "target"], "fplinux_cli.cli.nand.identify_target_nand"),
            (
                ["nand", "backup", "target", "nand.bin"],
                "fplinux_cli.cli.nand.backup_target_nand",
            ),
            (
                ["device-data", "prepare", "target", "--from-dump", "nand.bin"],
                "fplinux_cli.cli.device_data.prepare_device_data",
            ),
        ],
        ids=["build", "run", "console", "verify", "identify", "backup", "prepare"],
    )
    def test_explicit_phone_directory_reaches_every_store_consumer(
        self, dispatch_root: Path, arguments: list[str], callback_name: str
    ) -> None:
        """The parser forwards a chosen store and leaves the default store unspecified."""
        _events, selected = run_dispatch(
            dispatch_root, [*arguments, "--phone-data", "second phone"], callback_name
        )
        assert selected.call_args.kwargs["phone_data"] == Path("second phone")

        _events, ordinary = run_dispatch(dispatch_root, arguments, callback_name)
        assert ordinary.call_args.kwargs.get("phone_data") is None

    @pytest.mark.parametrize(
        ("arguments", "message"),
        [
            (
                ["dependencies", "restore"],
                "restore requires a snapshot directory or --release ID",
            ),
            (
                ["dependencies", "restore", "snapshot", "--release", "aaaaaaaaaaaa"],
                "restore --release cannot be combined with a directory or --inputs-only",
            ),
            (
                ["dependencies", "restore", "--release", "aaaaaaaaaaaa", "--inputs-only"],
                "restore --release cannot be combined with a directory or --inputs-only",
            ),
        ],
    )
    def test_invalid_dependency_restore_selection_stops_before_cache_access(
        self, arguments: list[str], message: str
    ) -> None:
        """Report incompatible user arguments without starting restoration or a traceback."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("invalid selection must not access cache state"),
            ),
            pytest.raises(SystemExit, match=message),
        ):
            cli.main()

    def test_package_forwards_the_selected_host_and_destination(self, dispatch_root: Path) -> None:
        """Keep the chosen asset name and output path under the packaging writer lock."""
        events, package = run_dispatch(
            dispatch_root,
            [
                "package",
                "target",
                "--host",
                "windows",
                "--output-directory",
                "archives",
            ],
            "fplinux_cli.packaging.archive.package_target",
        )
        assert events[0] == ("lock", dispatch_root / ".data", True, "package", "target", None)
        assert package.call_args.kwargs["host"] == "windows"
        assert package.call_args.kwargs["output_directory"] == Path("archives")

    @pytest.mark.parametrize(
        ("arguments", "callback_name", "locked"),
        [
            (["ci", "matrix"], "fplinux_cli.ci.commands.run_ci", False),
            (
                ["ci", "restore-inputs", "--directory", "inputs", "--snapshot", "a" * 64],
                "fplinux_cli.ci.commands.run_ci",
                True,
            ),
            (["_pre-push"], "fplinux_cli.quality.pre_push.check_pre_push", False),
            (
                ["inspect", "distribution", "archives"],
                "fplinux_cli.cli.inspect.inspect_distribution",
                False,
            ),
        ],
    )
    def test_release_stages_keep_readers_separate_from_cache_restoration(
        self, dispatch_root: Path, arguments: list[str], callback_name: str, *, locked: bool
    ) -> None:
        """Only restoration needs cache ownership among the new release stages."""
        events, _callback = run_dispatch(dispatch_root, arguments, callback_name)
        if locked:
            assert events == [
                ("lock", dispatch_root / ".data", True, "ci", None, None),
                "command",
                "unlock",
            ]
        else:
            assert events == ["command"]

    @pytest.mark.parametrize(
        ("options", "include_device_data"),
        [([], True), (["--without-device-data"], False)],
        ids=["ordinary-build", "without-device-data"],
    )
    def test_build_forwards_the_explicit_device_data_selection(
        self, dispatch_root: Path, options: list[str], *, include_device_data: bool
    ) -> None:
        """The public build option reaches snapshot policy while the normal cache lock is held."""
        events, build = run_dispatch(
            dispatch_root,
            ["build", "target", "--jobs", "1", *options],
            "fplinux_cli.build.workflow.build",
        )

        assert events == [
            ("lock", dispatch_root / ".data", True, "build", "target", None),
            "command",
            "unlock",
        ]
        assert build.call_args.args == ("target", 1)
        assert build.call_args.kwargs["include_device_data"] is include_device_data

    @pytest.mark.parametrize(
        ("arguments", "callback_name", "exclusive", "target", "profile"),
        [
            (["check"], "fplinux_cli.quality.checks.check", True, None, None),
            (
                ["test", "tests.small.environment.test_common"],
                "fplinux_cli.quality.testing.run_tests",
                True,
                None,
                None,
            ),
            (
                ["inspect", "bundle", "target"],
                "fplinux_cli.cli.inspect.inspect_bundle",
                False,
                "target",
                None,
            ),
            (
                ["inspect", "footprint", "target"],
                "fplinux_cli.cli.inspect.inspect_target_footprint",
                False,
                "target",
                None,
            ),
            (
                ["checksum", "demo-aport"],
                "fplinux_cli.alpine.checksum.checksum_aport",
                True,
                None,
                None,
            ),
            (
                ["format", "scripts/demo.py"],
                "fplinux_cli.quality.formatting.command.format_sources",
                True,
                None,
                None,
            ),
            (
                ["probe-build", "probe.c", "--output", ".data/tools/probe"],
                "fplinux_cli.build.probe.build_probe",
                True,
                None,
                None,
            ),
            (["prune", "--apply"], "fplinux_cli.cache.prune.operations.prune", True, None, None),
            (["run", "target"], "fplinux_cli.runtime.runner.run_target", False, "target", None),
            (["verify", "target"], "fplinux_cli.cli.runtime.verify_booted", False, "target", None),
            (
                ["console", "target"],
                "fplinux_cli.cli.runtime.console_target",
                False,
                "target",
                None,
            ),
            (
                ["nand", "backup", "nokia-ta1618", "backup.bin"],
                "fplinux_cli.cli.nand.backup_target_nand",
                False,
                "nokia-ta1618",
                None,
            ),
            (
                ["nand", "backup", "nokia-ta1618", "backup.bin", "--profile", "microsd-uboot"],
                "fplinux_cli.cli.nand.backup_target_nand",
                False,
                "nokia-ta1618",
                "microsd-uboot",
            ),
            (
                ["nand", "identify", "nokia-ta1618"],
                "fplinux_cli.cli.nand.identify_target_nand",
                False,
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
                ],
                "fplinux_cli.cli.device_data.prepare_device_data",
                True,
                "nokia-ta1618",
                None,
            ),
        ],
        ids=[
            "check",
            "test-tests.small.environment.test_common",
            "inspect-bundle-target",
            "inspect-footprint-target",
            "checksum-demo-aport",
            "format-scripts/demo.py",
            "probe-build-probe.c---output-.cache/tools/probe",
            "prune---apply",
            "run-target",
            "verify-target",
            "console-target",
            "nand-backup-nokia-ta1618-backup.bin",
            "nand-backup-nokia-ta1618-backup.bin---profile-microsd-uboot",
            "nand-identify-nokia-ta1618",
            "device-data-prepare-nokia-ta1618---from-dump-saved-nand.bin---jobs-2",
        ],
    )
    def test_dispatcher_chooses_the_required_lock_mode(  # noqa: PLR0913 -- fixture and independent dispatch expectations stay explicit.
        self,
        dispatch_root: Path,
        *,
        arguments: list[str],
        callback_name: str,
        exclusive: bool,
        target: str | None,
        profile: str | None,
    ) -> None:
        """Build-side commands request exclusive mode; consumers request shared mode."""
        events, callback = run_dispatch(dispatch_root, arguments, callback_name)
        assert (events) == (
            [
                ("lock", dispatch_root / ".data", exclusive, arguments[0], target, profile),
                "command",
                "unlock",
            ]
        )
        if callback_name == "fplinux_cli.cli.nand.backup_target_nand":
            assert (callback.call_args) == (
                mock.call(
                    "nokia-ta1618",
                    Path("backup.bin"),
                    profile=profile,
                    build_type="release",
                    phone_data=None,
                )
            )
        elif callback_name == "fplinux_cli.cli.nand.identify_target_nand":
            assert (callback.call_args) == (
                mock.call("nokia-ta1618", profile=None, build_type="release")
            )
        elif callback_name == "fplinux_cli.cli.device_data.prepare_device_data":
            assert (callback.call_args) == (
                mock.call(
                    "nokia-ta1618",
                    from_dump=Path("saved-nand.bin"),
                    from_originals=None,
                    events=None,
                    jobs=2,
                )
            )

    def test_named_originals_reach_prepare_under_the_exclusive_cache_lock(
        self, dispatch_root: Path
    ) -> None:
        """The saved-original source stays an explicit path through the public dispatcher."""
        events, callback = run_dispatch(
            dispatch_root,
            ["device-data", "prepare", "target", "--from-originals", "originals.json"],
            "fplinux_cli.cli.device_data.prepare_device_data",
        )

        assert events == [
            ("lock", dispatch_root / ".data", True, "device-data", "target", None),
            "command",
            "unlock",
        ]
        assert callback.call_args.args == ("target",)
        assert callback.call_args.kwargs["from_originals"] == Path("originals.json")
        assert callback.call_args.kwargs["from_dump"] is None
        assert callback.call_args.kwargs["events"] is None

    def test_format_forwards_only_the_explicit_paths(self, dispatch_root: Path) -> None:
        """Pass the ordered source selection through the exclusive command boundary."""
        _events, formatter = run_dispatch(
            dispatch_root,
            ["format", "scripts/tool.py", "README.md"],
            "fplinux_cli.quality.formatting.command.format_sources",
        )

        formatter.assert_called_once_with(["scripts/tool.py", "README.md"])

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["run", "target"], "fplinux_cli.runtime.runner.run_target"),
            (
                ["device-data", "prepare", "target"],
                "fplinux_cli.cli.device_data.prepare_device_data",
            ),
        ],
        ids=["run-target", "device-data-prepare-target"],
    )
    def test_loader_events_path_reaches_each_live_loader_command(
        self, dispatch_root: Path, arguments: list[str], callback_name: str
    ) -> None:
        """The output destination reaches the loader boundary as a filesystem path."""
        _events, callback = run_dispatch(
            dispatch_root, [*arguments, "--events", "loader-events.jsonl"], callback_name
        )
        assert (callback.call_args.kwargs["events"]) == (Path("loader-events.jsonl"))

    @pytest.mark.parametrize("trace_arguments", [[], ["--loader-trace"]], ids=["default", "trace"])
    def test_run_enables_loader_trace_only_when_requested(
        self, dispatch_root: Path, trace_arguments: list[str]
    ) -> None:
        """The public switch reaches the run backend without changing its default."""
        _events, callback = run_dispatch(
            dispatch_root,
            ["run", "target", *trace_arguments],
            "fplinux_cli.runtime.runner.run_target",
        )
        assert callback.call_args.kwargs.get("loader_trace", False) is bool(trace_arguments)

    @pytest.mark.parametrize(
        ("options", "expected"),
        [
            pytest.param([], 8192, id="default-eight-gib"),
            pytest.param(["--memory-mib", "4096"], 4096, id="explicit-four-gib"),
        ],
    )
    def test_build_forwards_the_memory_budget(
        self, dispatch_root: Path, options: list[str], expected: int
    ) -> None:
        """The build callback receives the parsed memory budget in MiB."""
        _events, build = run_dispatch(
            dispatch_root,
            ["build", "target", *options],
            "fplinux_cli.build.workflow.build",
        )

        assert build.call_args.kwargs["memory_mib"] == expected

    @pytest.mark.parametrize("value", ["0", "-1", "invalid"])
    def test_build_rejects_invalid_memory_before_locking(
        self, dispatch_root: Path, value: str
    ) -> None:
        """An invalid memory budget cannot create cache state or start a build."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", "build", "target", "--memory-mib", value]),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("invalid arguments must not lock"),
            ) as lock,
            contextlib.redirect_stderr(io.StringIO()),
            pytest.raises(SystemExit) as stopped,
        ):
            cli.main()

        assert stopped.value.code == 2
        lock.assert_not_called()
        assert not (dispatch_root / ".data").exists()

    @pytest.mark.parametrize(
        ("arguments", "callback_name"),
        [
            (["build", "target"], "fplinux_cli.build.workflow.build"),
            (["check", "kernel"], "fplinux_cli.quality.checks.check"),
            (["inspect", "bundle", "target"], "fplinux_cli.cli.inspect.inspect_bundle"),
            (["run", "target"], "fplinux_cli.runtime.runner.run_target"),
            (["console", "target"], "fplinux_cli.cli.runtime.console_target"),
            (["verify", "target"], "fplinux_cli.cli.runtime.verify_booted"),
            (
                ["nand", "identify", "target"],
                "fplinux_cli.cli.nand.identify_target_nand",
            ),
            (
                ["nand", "backup", "target", "nand.bin"],
                "fplinux_cli.cli.nand.backup_target_nand",
            ),
        ],
        ids=[
            "build-target",
            "check-kernel",
            "inspect-bundle-target",
            "run-target",
            "console-target",
            "verify-target",
            "nand-identify-target",
            "nand-backup-target-nand.bin",
        ],
    )
    @pytest.mark.parametrize("build_type", ["release", "debug"], ids=("release", "debug"))
    def test_build_type_defaults_and_explicit_selection_reach_every_consumer(
        self, dispatch_root: Path, arguments: list[str], callback_name: str, build_type: str
    ) -> None:
        """The same enum is independent of a boot profile at every selected-bundle boundary."""
        explicit = [
            *arguments,
            "--profile",
            "microsd-uboot",
            "--build-type",
            build_type,
        ]
        _events, callback = run_dispatch(dispatch_root, explicit, callback_name)
        assert (callback.call_args.kwargs["build_type"]) == (build_type)
        assert (callback.call_args.kwargs["profile"]) == ("microsd-uboot")
        _events, callback = run_dispatch(dispatch_root, arguments, callback_name)
        assert (callback.call_args.kwargs["build_type"]) == ("release")

    def test_unknown_build_type_is_rejected_before_locking(self) -> None:
        """An unsupported kernel type cannot reach build or device actions."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", "run", "target", "--build-type", "other"]),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
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
        self, dispatch_root: Path, extra: list[str], platform: str | None, compatible: str | None
    ) -> None:
        """The new-target command passes the platform and identity options to target creation."""
        callback = mock.Mock()
        arguments = ["target", "new", "hammer-lte", "--brand", "HAMMER", "--product", "LTE"]
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments, *extra]),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch(
                "fplinux_cli.manifests.paths.discover_targets", return_value=("nokia-ta1618",)
            ),
            mock.patch("fplinux_cli.cli.target_new.create_target", callback),
        ):
            cli.main()

        callback.assert_called_once_with(
            "hammer-lte",
            platform=platform,
            brand="HAMMER",
            product="LTE",
            compatible=compatible,
        )

    def test_device_data_prepare_rejects_a_nonpositive_job_limit_before_locking(
        self, dispatch_root: Path
    ) -> None:
        """Invalid worker limits cannot create cache state or start preparation."""
        with (
            mock.patch.object(
                sys,
                "argv",
                ["fplinux", "device-data", "prepare", "nokia-ta1618", "--jobs", "0"],
            ),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch(
                "fplinux_cli.manifests.paths.discover_targets", return_value=("nokia-ta1618",)
            ),
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
        assert not ((dispatch_root / ".data").exists())

    def test_check_forwards_the_kernel_worker_limit_without_changing_its_lock(
        self, dispatch_root: Path
    ) -> None:
        """Pass the requested kernel limit through the existing exclusive check boundary."""
        events, check = run_dispatch(
            dispatch_root, ["check", "kernel", "--jobs", "2"], "fplinux_cli.quality.checks.check"
        )

        assert (events) == (
            [
                ("lock", dispatch_root / ".data", True, "check", None, None),
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
            (["check", "tests"], ["tests"], False, 1),
            (["check", "python", "tests"], ["python", "tests"], False, 1),
            (["check", "--verbose"], [], True, 1),
        ],
        ids=[
            "check",
            "check-kernel",
            "check-docs",
            "check-tests",
            "check-python-tests",
            "check---verbose",
        ],
    )
    def test_check_defaults_match_the_selected_execution_boundary(
        self,
        dispatch_root: Path,
        *,
        arguments: list[str],
        scopes: list[str],
        verbose: bool,
        jobs: int,
    ) -> None:
        """Use three kernel workers normally and one when no parallel work can run."""
        _events, check = run_dispatch(dispatch_root, arguments, "fplinux_cli.quality.checks.check")
        check.assert_called_once_with(
            scopes,
            profile=None,
            build_type="release",
            verbose=verbose,
            no_cache=False,
            jobs=jobs,
        )

    def test_profile_is_recorded_in_the_global_cache_lock_owner(self, dispatch_root: Path) -> None:
        """A blocked build identifies its profile without splitting the global lock."""
        events, build = run_dispatch(
            dispatch_root,
            ["build", "target", "--profile", "microsd-uboot"],
            "fplinux_cli.build.workflow.build",
        )

        assert (events) == (
            [
                (
                    "lock",
                    dispatch_root / ".data",
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
            (
                ["package", "target", "--profile", "microsd-uboot"],
                "fplinux_cli.packaging.archive.package_target",
            ),
            (
                ["run", "target", "--profile", "microsd-uboot"],
                "fplinux_cli.runtime.runner.run_target",
            ),
            (
                ["console", "target", "--profile", "microsd-uboot", "--exec", "id"],
                "fplinux_cli.cli.runtime.console_target",
            ),
        ],
        ids=[
            "package-target---profile-microsd-uboot",
            "run-target---profile-microsd-uboot",
            "console-target---profile-microsd-uboot---exec-id",
        ],
    )
    def test_profile_consumers_use_the_selected_shared_lock_identity(
        self, dispatch_root: Path, arguments: list[str], callback_name: str
    ) -> None:
        """Each consumer retains its selected bundle slot under the required cache lock."""
        events, callback = run_dispatch(dispatch_root, arguments, callback_name)

        assert (events) == (
            [
                (
                    "lock",
                    dispatch_root / ".data",
                    arguments[0] == "package",
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
        ("options", "host"),
        [
            ([], "linux"),
            (["--host", "windows"], "windows"),
        ],
        ids=["linux", "windows"],
    )
    def test_package_host_options_reach_the_backend_under_the_required_lock(
        self,
        dispatch_root: Path,
        *,
        options: list[str],
        host: str,
    ) -> None:
        """Both native archive hosts prepare their runtime under an exclusive cache lock."""
        events, package = run_dispatch(
            dispatch_root,
            ["package", "target", *options],
            "fplinux_cli.packaging.archive.package_target",
        )

        assert events == [
            ("lock", dispatch_root / ".data", True, "package", "target", None),
            "command",
            "unlock",
        ]
        package.assert_called_once_with(
            "target",
            profile=None,
            boot=None,
            host=host,
            output_directory=None,
        )

    @pytest.mark.parametrize(
        ("arguments", "callback_name", "expected_call"),
        [
            (
                ["run", "nokia-ta1618", "--boot", "microsd"],
                "fplinux_cli.runtime.runner.run_target",
                mock.call(
                    "nokia-ta1618", profile=None, boot="microsd", build_type="release", events=None
                ),
            ),
            (
                ["package", "nokia-ta1618", "--boot", "microsd"],
                "fplinux_cli.packaging.archive.package_target",
                mock.call(
                    "nokia-ta1618",
                    profile=None,
                    boot="microsd",
                    host="linux",
                    output_directory=None,
                ),
            ),
            (
                ["run", "target", "--boot", "microsd"],
                "fplinux_cli.runtime.runner.run_target",
                mock.call(
                    "target", profile=None, boot="microsd", build_type="release", events=None
                ),
            ),
        ],
        ids=[
            "run-nokia-ta1618---boot-microsd",
            "package-nokia-ta1618---boot-microsd",
            "run-target---boot-microsd",
        ],
    )
    def test_microsd_boot_selector_locks_the_single_selected_context(
        self, dispatch_root: Path, arguments: list[str], callback_name: str, expected_call: object
    ) -> None:
        """The boot selector keeps its target and chooses one profile cache slot."""
        command, target = arguments[:2]
        events, callback = run_dispatch(dispatch_root, arguments, callback_name)

        assert (events) == (
            [
                (
                    "lock",
                    dispatch_root / ".data",
                    command == "package",
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
        self, dispatch_root: Path, command: str
    ) -> None:
        """One run or package invocation selects either a boot mode or a profile, not both."""
        with (
            mock.patch.object(
                sys,
                "argv",
                ["fplinux", command, "target", "--boot", "microsd", "--profile", "microsd-uboot"],
            ),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
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
        assert not ((dispatch_root / ".data").exists())

    @pytest.mark.parametrize(
        ("command", "callback"),
        [
            ("build", "fplinux_cli.build.workflow.build"),
            ("console", "fplinux_cli.cli.runtime.console_target"),
            ("verify", "fplinux_cli.cli.runtime.verify_booted"),
        ],
        ids=["build", "console", "verify"],
    )
    def test_explicit_default_reuses_the_implicit_context(
        self, dispatch_root: Path, command: str, callback: str
    ) -> None:
        """The alias reaches each consumer with the same profile and lock identity."""
        implicit_events, implicit = run_dispatch(dispatch_root, [command, "target"], callback)
        explicit_events, explicit = run_dispatch(
            dispatch_root, [command, "target", "--profile", "default"], callback
        )
        assert (explicit_events) == (implicit_events)
        assert (explicit.call_args) == (implicit.call_args)

    def test_named_profile_keeps_explicit_check_scopes(self, dispatch_root: Path) -> None:
        """Both boot modes can run the same source checks without selecting Kbuild."""
        _events, check = run_dispatch(
            dispatch_root,
            ["check", "python", "source", "--profile", "microsd-uboot"],
            "fplinux_cli.quality.checks.check",
        )
        check.assert_called_once_with(
            ["python", "source"],
            profile="microsd-uboot",
            build_type="release",
            verbose=False,
            no_cache=False,
            jobs=1,
        )

    def test_verify_forwards_the_selected_profile(self, dispatch_root: Path) -> None:
        """A microSD verification cannot silently resolve the default bundle."""
        events, verify = run_dispatch(
            dispatch_root,
            ["verify", "target", "--profile", "microsd-uboot"],
            "fplinux_cli.cli.runtime.verify_booted",
        )
        assert (events) == (
            [
                ("lock", dispatch_root / ".data", False, "verify", "target", "microsd-uboot"),
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
        self, dispatch_root: Path, arguments: tuple[str, ...], message: str
    ) -> None:
        """Only a public profile name reaches the cache lock or deferred cleanup."""
        stderr = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("invalid profile must not take the cache lock"),
            ) as lock,
            mock.patch(
                "fplinux_cli.cache.prune.operations.discard_obsolete_rootfs",
                side_effect=AssertionError("invalid profile must not prune rootfs"),
            ) as rootfs_gc,
            mock.patch(
                "fplinux_cli.cache.prune.operations.discard_superseded_profile_logs",
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
        assert not ((dispatch_root / ".data").exists())

    @pytest.mark.parametrize(
        ("command", "target", "expected_build_cleanup"),
        [("build", "nokia", True), ("check", None, False)],
        ids=["build", "check"],
    )
    def test_failed_profile_commands_keep_the_initial_data_root_for_cleanup(
        self,
        dispatch_root: Path,
        monkeypatch: pytest.MonkeyPatch,
        *,
        command: str,
        target: str | None,
        expected_build_cleanup: bool,
    ) -> None:
        """A failed action still reaches the cleanup boundary with its selected context."""
        profile = "microsd-uboot"
        cache = dispatch_root / ".data"
        arguments = argparse.Namespace(
            command=command,
            target=target,
            profile=profile,
            list_scopes=False,
        )

        def fail_after_changing_environment() -> None:
            monkeypatch.setenv("FPLINUX_DATA_DIR", str(dispatch_root / "different-data"))
            message = "forced profile failure"
            raise SystemExit(message)

        action = mock.Mock(side_effect=fail_after_changing_environment)
        with (
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                return_value=contextlib.nullcontext(),
            ),
            mock.patch(
                "fplinux_cli.cache.prune.operations.discard_obsolete_rootfs"
            ) as rootfs_cleanup,
            mock.patch("fplinux_cli.cache.prune.operations.discard_obsolete_apks") as apks_cleanup,
            mock.patch(
                "fplinux_cli.cache.prune.operations.discard_superseded_profile_logs",
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
        assert not (dispatch_root / "different-data").exists()

    @pytest.mark.parametrize(
        ("arguments", "prune_arguments"),
        [(["check", "--list"], None), (["prune"], False)],
        ids=["check---list", "prune"],
    )
    def test_check_list_and_dry_prune_dispatch_without_the_cache_lock(
        self, dispatch_root: Path, *, arguments: list[str], prune_arguments: bool | None
    ) -> None:
        """The dispatcher neither locks nor creates the cache for the two no-work commands.

        `check --list` runs for real. The prune operation is replaced, so this
        checks only that the dry-run choice reaches it; prune's own read-only behavior
        is not exercised here.
        """
        with (
            mock.patch.object(sys, "argv", ["fplinux", *arguments]),
            mock.patch.object(lifecycle, "ROOT", dispatch_root),
            mock.patch("fplinux_cli.manifests.paths.discover_targets", return_value=("target",)),
            mock.patch.object(
                lifecycle,
                "cache_lock",
                side_effect=AssertionError("this path must not lock"),
            ) as lock,
            mock.patch("fplinux_cli.cache.prune.operations.prune") as prune,
            mock.patch("builtins.print"),
        ):
            cli.main()
        lock.assert_not_called()
        if prune_arguments is not None:
            prune.assert_called_once_with(apply=prune_arguments)
        assert not ((dispatch_root / ".data").exists())
