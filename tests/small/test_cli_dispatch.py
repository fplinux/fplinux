# SPDX-License-Identifier: GPL-2.0-only
"""In-process CLI dispatcher selection of the one cache lock."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import __main__ as cli
from fplinux_cli import alpine_state
from fplinux_cli import prune as prune_module

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
        cases = (
            (["build", "target", "--jobs", "1"], "build", True, "target"),
            (["check"], "check", True, None),
            (["checksum", "demo-aport"], "checksum_aport", True, None),
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
                        ("lock", self.root / ".cache", exclusive, arguments[0], target, None),
                        "command",
                    ],
                )
                callback.assert_called_once()

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

    def test_profile_is_recorded_in_the_global_cache_lock_owner(self) -> None:
        """A blocked build identifies its profile without splitting the global lock."""
        events, build = self._run(
            ["build", "target", "--profile", "usb-host-lab"],
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
                    "usb-host-lab",
                ),
                "command",
            ],
        )
        self.assertEqual(build.call_args.kwargs["profile"], "usb-host-lab")

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
        """The boot selector chooses its profile slot before taking the shared lock."""
        for command, callback_name in (("run", "run_target"), ("package", "package_target")):
            arguments = [command, "nokia-ta1618", "--boot", "microsd"]
            if command == "package":
                arguments.append("--candidate")
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
                            "nokia-ta1618",
                            "microsd-uboot",
                        ),
                        "command",
                    ],
                )
                self.assertEqual(callback.call_args.kwargs["boot"], "microsd")
                self.assertIsNone(callback.call_args.kwargs["profile"])

    def test_microsd_boot_does_not_fall_back_to_another_target(self) -> None:
        """Reject the unsupported mode before a lock or command touches another target."""
        with (
            mock.patch.object(sys, "argv", ["fplinux", "run", "target", "--boot", "microsd"]),
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(
                cli,
                "discover_targets",
                return_value=("target", "nokia-ta1618"),
            ),
            mock.patch.object(
                cli,
                "cache_lock",
                side_effect=AssertionError("unsupported boot mode must not take the cache lock"),
            ) as lock,
            mock.patch.object(cli, "run_target") as run,
            self.assertRaisesRegex(SystemExit, "not available for target target"),
        ):
            cli.main()

        lock.assert_not_called()
        run.assert_not_called()

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

    def _write_profile_run(
        self,
        command: str,
        *,
        target: str | None,
        profile: str | None,
        index: int,
        malformed: bool = False,
    ) -> Path:
        """Write one synthetic reporter record independent of prune's matcher."""
        run_name = f"20260820T0508{index:02d}Z-p{index}"
        if command == "build":
            if target is None:
                self.fail("build log fixture needs a target")
            identity = f"logs/build/{target}"
            label = f"build {target}"
            if profile is not None:
                identity += f"/profiles/{profile}"
                label += f"/profiles/{profile}"
        else:
            identity = "logs/check"
            label = "check"
            if profile is not None:
                identity += f"/profiles/{profile}"
                label += f" profiles/{profile}"
        run = self.root / ".cache" / identity / run_name
        run.mkdir(parents=True)
        contents: object = (
            {"display_root": f".cache/{identity}/{run_name}", "label": label, "parent": None}
            if not malformed
            else {"unexpected": "keep"}
        )
        (run / "run.json").write_text(json.dumps(contents), encoding="utf-8")
        return run

    def test_failed_profile_build_and_check_bound_only_valid_profile_logs(self) -> None:
        """Dispatch-finally bounds profile state after failures without touching valid slots."""
        profile = "usb-host-lab"
        default_runs = [
            self._write_profile_run("build", target="nokia", profile=None, index=index)
            for index in range(11)
        ]
        malformed_build = self._write_profile_run(
            "build",
            target="nokia",
            profile=profile,
            index=99,
            malformed=True,
        )
        malformed_check = self._write_profile_run(
            "check",
            target=None,
            profile=profile,
            index=99,
            malformed=True,
        )

        def fail_after_recording(command: str, target: str | None, index: int) -> None:
            self._write_profile_run(command, target=target, profile=profile, index=index)
            message = "forced profile failure"
            raise SystemExit(message)

        apks = self.root / ".cache" / alpine_state.PACKAGE_CACHE_DIRECTORY
        obsolete_apk = apks / "fplinux-profile-x"
        obsolete_apk.mkdir(parents=True)
        current_apks = ("fplinux-base", "fplinux-profile-y", "fplinux-bundle-host")
        for package in current_apks:
            (apks / package).mkdir()

        def load_target(_target: str, selected_profile: str | None = None) -> dict[str, object]:
            return {"platform": "platform", "profile": selected_profile}

        def selected_packages(
            _platform: dict[str, object], config: dict[str, object]
        ) -> tuple[str, ...]:
            return ("fplinux-profile-y",) if config["profile"] is not None else ("fplinux-base",)

        def bundle_packages(
            _platform: dict[str, object], config: dict[str, object], _rootfs: tuple[str, ...]
        ) -> tuple[str, ...]:
            return ("fplinux-bundle-host",) if config["profile"] is not None else ()

        with (
            mock.patch.object(cli, "ROOT", self.root),
            mock.patch.object(prune_module, "discover_targets", return_value=("nokia",)),
            mock.patch.object(prune_module, "discover_profiles", return_value=(profile,)),
            mock.patch.object(prune_module, "load_target", side_effect=load_target),
            mock.patch.object(prune_module, "load_platform", return_value={}),
            mock.patch.object(
                alpine_state,
                "selected_packages",
                side_effect=selected_packages,
            ),
            mock.patch.object(
                alpine_state,
                "bundle_packages",
                side_effect=bundle_packages,
            ),
            mock.patch(
                "fplinux_cli.__main__.discard_obsolete_rootfs",
                wraps=prune_module.discard_obsolete_rootfs,
            ) as rootfs_gc,
            mock.patch(
                "fplinux_cli.__main__.discard_obsolete_apks",
                wraps=prune_module.discard_obsolete_apks,
            ) as apks_gc,
        ):
            for command, target in (("build", "nokia"), ("check", None)):
                for index in range(11):
                    arguments = argparse.Namespace(
                        command=command,
                        target=target,
                        profile=profile,
                        list_scopes=False,
                    )

                    def action(
                        command: str = command,
                        target: str | None = target,
                        index: int = index,
                    ) -> None:
                        fail_after_recording(command, target, index)

                    with self.assertRaisesRegex(SystemExit, "forced profile failure"):
                        cli._dispatch_with_cache_lock(  # noqa: SLF001 -- lock lifecycle boundary.
                            arguments,
                            action,
                        )

        self.assertEqual(rootfs_gc.call_args_list, [mock.call(self.root / ".cache")] * 11)
        self.assertEqual(apks_gc.call_args_list, [mock.call(self.root / ".cache")] * 11)
        self.assertFalse(obsolete_apk.exists())
        self.assertTrue(all((apks / package).is_dir() for package in current_apks))
        for command, _target, malformed in (
            ("build", "nokia", malformed_build),
            ("check", None, malformed_check),
        ):
            if command == "build":
                root = self.root / ".cache/logs/build/nokia/profiles" / profile
            else:
                root = self.root / ".cache/logs/check/profiles" / profile
            valid_runs = [path for path in root.iterdir() if path != malformed]
            self.assertEqual(len(valid_runs), 10)
            self.assertTrue(malformed.is_dir())
        self.assertTrue(all(path.is_dir() for path in default_runs))

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
