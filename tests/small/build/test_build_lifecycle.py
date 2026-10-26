# SPDX-License-Identifier: GPL-2.0-only
"""Build-command cache reuse, validation and controlled container execution."""

from __future__ import annotations

import contextlib
import functools
import io
import json
import os
from collections import Counter
from dataclasses import replace
from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.build.reuse as bundles_commands
import fplinux_cli.build.workflow as build_commands
import fplinux_cli.cache.prune.operations as prune_operations
import fplinux_cli.environment.kern as kern_env
import fplinux_cli.reporting.run as output
import fplinux_cli.workspace.build_inputs as workspace_inputs
import fplinux_cli.workspace.staging as workspace_staging
import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import bundle_pointer, publish_current_bundle
from fplinux_cli.environment import definition, setup
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests import releases, targets

from tests.small.build.command_fixtures import (
    command_bundle as command_bundle,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.build.command_fixtures import CommandBundleContext


class BuildLifecycleTests:
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_release_debug_release_hits_keep_both_types_and_ignore_resource_limits(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Switching type reuses its own generation without starting build work."""
        debug = command_bundle.create_generation("d" * 64, build_type="debug")
        publish_current_bundle(command_bundle.output, "phone", debug, build_type="debug")
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(output, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=command_bundle.target_config),
            mock.patch.object(releases, "load_release", return_value=command_bundle.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(
                definition, "load_environment_lock", return_value=command_bundle.lock
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                kern_env,
                "kern_available",
                side_effect=AssertionError("cache hit must not inspect Kern"),
            ),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                side_effect=AssertionError("cache hit must not stage a workspace"),
            ),
            mock.patch.object(
                workspace_staging,
                "discard_staged_workspace_snapshot",
                side_effect=AssertionError("cache hit must not discard an unstaged workspace"),
            ),
            mock.patch.object(prune_operations, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(prune_operations, "discard_obsolete_apks") as apks_gc,
        ):
            for build_type, jobs, memory_mib in (
                ("release", 1, 2048),
                ("debug", 8, 2048),
                ("release", 8, 4096),
            ):
                old = command_bundle.create_generation("b" * 64, build_type=build_type)
                stdout = io.StringIO()
                journals_before = set((command_bundle.cache / "logs").rglob("run.json"))
                with contextlib.redirect_stdout(stdout):
                    output.run_entrypoint(
                        functools.partial(
                            build_commands.build,
                            "phone",
                            jobs,
                            memory_mib=memory_mib,
                            verbose=True,
                            offline=True,
                            build_type=build_type,
                        )
                    )

                assert (f"build phone --build-type {build_type}: OK (cached)") in (
                    stdout.getvalue()
                )
                expected = command_bundle.bundle_path if build_type == "release" else debug
                assert (str(expected.relative_to(command_bundle.root))) in (stdout.getvalue())
                assert not (old.exists())
                assert command_bundle.bundle_path.is_dir()
                assert debug.is_dir()
                new_journals = set((command_bundle.cache / "logs").rglob("run.json")) - (
                    journals_before
                )
                assert len(new_journals) == 1
                metadata = json.loads(new_journals.pop().read_text())
                assert metadata["build_type"] == build_type
                assert metadata["status"] == "success"
            assert (rootfs_gc.call_args_list) == ([mock.call(command_bundle.cache)] * 3)
            assert (apks_gc.call_args_list) == ([mock.call(command_bundle.cache)] * 3)

    def test_build_result_ignores_closed_stdout_pipe(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """A closed output consumer must not turn a valid build result into failure."""

        class BrokenPipeStream(io.StringIO):
            def flush(self) -> None:
                raise BrokenPipeError

        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            contextlib.redirect_stdout(BrokenPipeStream()),
        ):
            build_commands._print_build_result(  # noqa: SLF001
                "phone",
                command_bundle.bundle,
                {"image": "image/ramboot.bin"},
                cached=False,
            )

    @pytest.mark.parametrize(
        ("relative", "mutation"),
        [
            pytest.param("image/ramboot.bin", "bytes", id="image-bytes"),
            pytest.param("host/fplinux-usb-keyboard", "bytes", id="keyboard-bytes"),
            pytest.param("host/fplinux-usb-keyboard", "missing", id="missing-keyboard"),
            pytest.param("image/ramboot.bin", "mode", id="image-mode"),
            pytest.param("host/fplinux-usb-keyboard", "mode", id="keyboard-mode"),
        ],
    )
    def test_changed_or_missing_bundle_files_are_cache_misses(
        self, command_bundle: CommandBundleContext, relative: str, mutation: str
    ) -> None:
        """Cache reuse requires every recorded payload byte and file mode to match."""
        identity = bundles_commands.BuildIdentity(
            command_bundle.snapshot.recipe,
            "e" * 64,
            "a" * 64,
            command_bundle.signing_key,
        )
        with mock.patch.object(common, "ROOT", command_bundle.root):
            source = command_bundle.bundle_path / relative
            if mutation == "bytes":
                original = source.read_bytes()
                source.write_bytes(bytes([original[0] ^ 0x20]) + original[1:])
            elif mutation == "missing":
                source.unlink()
            else:
                source.chmod(0o600)
            assert (
                bundles_commands.matching_target_bundle("phone", identity, "image/ramboot.bin")
                is None
            )

    def test_exact_bundle_identity_and_image_are_a_reusable_hit(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Reuse a resolved generation only when its identity and image bytes match."""
        identity = bundles_commands.BuildIdentity(
            command_bundle.snapshot.recipe,
            "e" * 64,
            "a" * 64,
            command_bundle.signing_key,
        )
        with mock.patch.object(common, "ROOT", command_bundle.root):
            matched = bundles_commands.matching_target_bundle(
                "phone",
                identity,
                "image/ramboot.bin",
            )

        if matched is None:
            pytest.fail("an exact bundle was not reusable")
        bundle, manifest = matched
        assert (bundle) == (command_bundle.bundle)
        assert (manifest) == (json.loads(command_bundle.bundle.manifest_bytes))


    @pytest.mark.parametrize(
        ("field", "changed"),
        [
            pytest.param("workspace_digest", "d" * 64, id="workspace"),
            pytest.param("environment_recipe", "f" * 64, id="environment-recipe"),
            pytest.param("environment_content", "b" * 64, id="environment-content"),
            pytest.param("apk_signing_key", "8" * 64, id="signing-key"),
        ],
    )
    def test_each_build_identity_mismatch_is_a_cache_miss(
        self, command_bundle: CommandBundleContext, field: str, changed: str
    ) -> None:
        """Reject a generation when any host-visible causal identity changed."""
        identity = bundles_commands.BuildIdentity(
            changed if field == "workspace_digest" else "c" * 64,
            changed if field == "environment_recipe" else "e" * 64,
            changed if field == "environment_content" else "a" * 64,
            changed if field == "apk_signing_key" else command_bundle.signing_key,
        )
        with mock.patch.object(common, "ROOT", command_bundle.root):
            assert (
                bundles_commands.matching_target_bundle("phone", identity, "image/ramboot.bin")
                is None
            )

    def test_build_miss_requires_host_validation_after_container_success(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Container exit zero is insufficient without an exact published generation."""
        workspace = command_bundle.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = command_bundle.create_generation("b" * 64)
        command_bundle.clear_current_bundle()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(output, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=command_bundle.target_config),
            mock.patch.object(releases, "load_release", return_value=command_bundle.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(
                definition, "load_environment_lock", return_value=command_bundle.lock
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(
                build_commands,
                "require_environment",
                return_value=(
                    "kern",
                    command_bundle.cache / "environment/rootfs",
                    EnvironmentState("e" * 64, "b" * 64, "a" * 64),
                ),
            ),
            mock.patch.object(kern_env, "kern_environment", return_value={}),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(build_commands, "captured_build_inputs", return_value=[]),
            mock.patch.object(workspace_staging, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(prune_operations, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(prune_operations, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True),
            pytest.raises(SystemExit, match="without publishing an exact valid current bundle"),
        ):
            output.run_entrypoint(lambda: build_commands.build("phone", 4))

        assert not (bundle_pointer(command_bundle.output, "phone").exists())
        assert old.exists()
        discard.assert_called_once_with(command_bundle.snapshot, workspace)
        rootfs_gc.assert_not_called()
        apks_gc.assert_not_called()
        metadata = next((command_bundle.root / ".cache/logs/build/phone").rglob("run.json"))
        assert (json.loads(metadata.read_text(encoding="utf-8"))["status"]) == ("failed")

    @pytest.mark.parametrize(
        ("memory_mib", "status"),
        [
            pytest.param(2048, 137, id="two-gib-limit"),
            pytest.param(12288, 137, id="twelve-gib-limit"),
            pytest.param(2048, 1, id="other-container-failure"),
        ],
    )
    def test_build_memory_failure_reports_selected_budget_and_preserves_exit(
        self,
        command_bundle: CommandBundleContext,
        capsys: pytest.CaptureFixture[str],
        memory_mib: int,
        status: int,
    ) -> None:
        """A confirmed limit failure explains the build control without changing its status."""
        command_bundle.clear_current_bundle()
        workspace = command_bundle.cache / "workspace"
        workspace.mkdir()
        reporter = output.RunReporter(
            "build", command_bundle.cache / "test-logs", "test", verbose=False
        )
        failure = SystemExit(status)

        def fail_workload(stage: output.Stage, command: list[str], **_kwargs: object) -> None:
            assert command[command.index("--memory") + 1] == f"{memory_mib}m"
            stage.write(
                b"kern: the workload was killed by the kernel's OOM killer "
                b"against this box's own memory cap.\n"
            )
            raise failure

        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(releases, "load_release", return_value=command_bundle.release),
            mock.patch.object(
                workspace_inputs, "target_workspace_snapshot", return_value=command_bundle.snapshot
            ),
            mock.patch.object(
                definition, "load_environment_lock", return_value=command_bundle.lock
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                build_commands,
                "require_environment",
                return_value=(
                    "kern",
                    command_bundle.cache / "environment/rootfs",
                    EnvironmentState("e" * 64, "b" * 64, "a" * 64),
                ),
            ),
            mock.patch.object(kern_env, "kern_environment", return_value={}),
            mock.patch.object(
                workspace_staging, "stage_workspace_snapshot", return_value=workspace
            ),
            mock.patch.object(build_commands, "captured_build_inputs", return_value=[]),
            mock.patch.object(workspace_staging, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=fail_workload),
            pytest.raises(SystemExit) as raised,
        ):
            build_commands.build("phone", 4, memory_mib=memory_mib, reporter=reporter)

        assert raised.value is failure
        stderr = capsys.readouterr().err
        hint = f"Kern memory limit reached ({memory_mib} MiB)."
        assert stderr.count(hint) == int(status == 137)
        assert ("Increase build --memory-mib" in stderr) == (status == 137)
        assert "--jobs" not in stderr
        log = next(reporter.root.glob("*-container.log")).read_text()
        assert log.count(hint) == int(status == 137)
        discard.assert_called_once_with(command_bundle.snapshot, workspace)

    @pytest.mark.parametrize(
        ("memory_mib", "expected_memory", "build_type"),
        [
            pytest.param(None, "8192m", "release", id="default-eight-gib-release"),
            pytest.param(4096, "4096m", "debug", id="explicit-four-gib-debug"),
        ],
    )
    def test_successful_build_uses_memory_budget_and_discards_superseded_after_host_validation(
        self,
        command_bundle: CommandBundleContext,
        memory_mib: int | None,
        expected_memory: str,
        build_type: str,
    ) -> None:
        """Retain old generations until the container result validates on the host."""
        workspace = command_bundle.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = command_bundle.create_generation("b" * 64, build_type=build_type)
        command_bundle.clear_current_bundle()
        result_bundle = command_bundle.bundle_path
        if build_type == "debug":
            result_bundle = command_bundle.create_generation("d" * 64, build_type="debug")

        def publish_result(_stage: output.Stage, command: list[str], **_kwargs: object) -> None:
            assert command[command.index("--memory") + 1] == expected_memory
            publish_current_bundle(
                command_bundle.output, "phone", result_bundle, build_type=build_type
            )

        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(output, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=command_bundle.target_config),
            mock.patch.object(releases, "load_release", return_value=command_bundle.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(
                definition, "load_environment_lock", return_value=command_bundle.lock
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(
                build_commands,
                "require_environment",
                return_value=(
                    "kern",
                    command_bundle.cache / "environment/rootfs",
                    EnvironmentState("e" * 64, "b" * 64, "a" * 64),
                ),
            ),
            mock.patch.object(kern_env, "kern_environment", return_value={}),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(build_commands, "captured_build_inputs", return_value=[]),
            mock.patch.object(workspace_staging, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(prune_operations, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(prune_operations, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=publish_result),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                if memory_mib is None:
                    output.run_entrypoint(
                        lambda: build_commands.build("phone", 4, build_type=build_type)
                    )
                else:
                    output.run_entrypoint(
                        lambda: build_commands.build(
                            "phone", 4, memory_mib=memory_mib, build_type=build_type
                        )
                    )

        assert (f"build phone --build-type {build_type}: OK") in (stdout.getvalue())
        assert (f"build phone --build-type {build_type}: OK (cached)") not in (stdout.getvalue())
        assert not (old.exists())
        discard.assert_called_once_with(command_bundle.snapshot, workspace)
        rootfs_gc.assert_called_once_with(command_bundle.cache)
        apks_gc.assert_called_once_with(command_bundle.cache)
        metadata = next((command_bundle.root / ".cache/logs/build/phone").rglob("run.json"))
        recorded = json.loads(metadata.read_text(encoding="utf-8"))
        assert recorded["status"] == "success"
        assert recorded["build_type"] == build_type

    def test_offline_build_miss_propagates_offline_environment_policy_without_staging(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Propagate the unavailable offline environment before staging build sources."""
        command_bundle.clear_current_bundle()
        with (
            mock.patch.object(common, "ROOT", command_bundle.root),
            mock.patch.object(output, "ROOT", command_bundle.root),
            mock.patch.object(targets, "load_target", return_value=command_bundle.target_config),
            mock.patch.object(releases, "load_release", return_value=command_bundle.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=command_bundle.snapshot,
            ),
            mock.patch.object(
                definition, "load_environment_lock", return_value=command_bundle.lock
            ),
            mock.patch.object(definition, "environment_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(
                build_commands,
                "require_environment",
                side_effect=SystemExit(
                    "offline execution requires the current pinned environment"
                ),
            ) as require_environment,
            mock.patch.object(
                setup,
                "setup",
                side_effect=AssertionError("offline build must not set up an environment"),
            ),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                side_effect=AssertionError(
                    "offline environment failure must not stage a workspace"
                ),
            ),
            pytest.raises(
                SystemExit, match="offline execution requires the current pinned environment"
            ),
        ):
            output.run_entrypoint(lambda: build_commands.build("phone", 4, offline=True))

        assert not (bundle_pointer(command_bundle.output, "phone").exists())
        require_environment.assert_called_once_with(offline=True, reporter=mock.ANY)

    def test_build_argv_has_explicit_memory_budget_and_narrow_mounts(
        self, command_bundle: CommandBundleContext
    ) -> None:
        """Request a 2 GiB build budget without exposing broad cache mounts."""
        roots = {
            "workspace": command_bundle.root / "workspace",
            "downloads": command_bundle.root / "cache/downloads",
            "ccache": command_bundle.root / "cache/ccache",
            "host_tools": command_bundle.root / "cache/host-tools",
            "apk_signing": command_bundle.root / "cache/apk-signing",
            "apks": command_bundle.root / "cache/apks",
            "rootfs": command_bundle.root / "cache/rootfs",
            "linux": command_bundle.root / "cache/linux",
            "output": command_bundle.root / "cache/out",
            "logs": command_bundle.root / "cache/logs/build/run",
        }
        with mock.patch.dict(os.environ, {}, clear=True):
            command = build_commands._build_command(  # noqa: SLF001
                "/usr/bin/kern",
                target="phone",
                jobs=6,
                memory_mib=2048,
                environment_rootfs=command_bundle.cache / "environment/rootfs",
                snapshot=command_bundle.snapshot,
                **roots,
                profile=None,
                build_type="release",
                log_environment={"FPLINUX_LOG_ROOT": "/logs"},
                environment_recipe="e" * 64,
                environment_content="a" * 64,
            )

        mounts = [command[index + 1] for index, value in enumerate(command) if value == "--volume"]
        # No destination is nested in another, so mount order has no effect.
        assert Counter(mounts) == Counter(
            [
                f"{roots['downloads']}:/cache/downloads",
                f"{roots['ccache']}:/cache/ccache",
                f"{roots['host_tools']}:/cache/host-tools",
                f"{roots['apk_signing']}:/cache/apk-signing",
                f"{roots['apks']}:/cache/apks",
                f"{roots['rootfs']}:/cache/rootfs",
                f"{roots['linux']}:/cache/linux",
                f"{roots['output']}:/out",
                f"{roots['logs']}:/logs",
                f"{roots['workspace']}:/workspace:ro",
            ]
        )
        assert ("--read-only") in (command)
        assert command[command.index("--rootfs") + 1] == str(
            command_bundle.cache / "environment/rootfs"
        )
        assert "--uid-range" in command
        assert "--image" not in command
        assert "--pull" not in command
        assert (
            "PATH=/opt/quality/bin:/opt/quality/node/bin:"
            "/opt/quality/node-tools/node_modules/.bin:/usr/local/sbin:/usr/local/bin:"
            "/usr/sbin:/usr/bin:/sbin:/bin"
        ) in command
        assert ("--privileged") in (command)
        assert ("--memory") in (command)
        assert (command[command.index("--memory") + 1]) == ("2048m")
        assert not (any(mount.split(":", 2)[1] == "/cache" for mount in mounts))
        assert ("FPLINUX_ENVIRONMENT_RECIPE=" + "e" * 64) in (command)
        assert ("FPLINUX_ENVIRONMENT_CONTENT=" + "a" * 64) in (command)
        build = command[command.index("--") + 1 :]
        assert (build[:3]) == (["python3", "-m", "fplinux_cli.build"])
        # The in-container parser accepts these options in any order; compare flag/value pairs.
        assert Counter(zip(build[3::2], build[4::2], strict=True)) == Counter(
            [("--target", "phone"), ("--build-type", "release"), ("--jobs", "6")]
        )
        assert (command[:2]) == (["/usr/bin/kern", "box"])
        network = command.index("--network")
        assert (command[network + 1]) == ("none")
