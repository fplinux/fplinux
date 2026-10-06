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
from unittest import mock

import fplinux_cli.cache.prune.operations as prune_operations
import fplinux_cli.cli.build as build_commands
import fplinux_cli.cli.bundles as bundles_commands
import fplinux_cli.environment.kern as kern_env
import fplinux_cli.reporting.run as output
import fplinux_cli.workspace.build_inputs as workspace_inputs
import fplinux_cli.workspace.staging as workspace_staging
import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import bundle_pointer, publish_current_bundle
from fplinux_cli.environment import image_store, images, setup
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.manifests import releases, targets

from tests.small.build.command_fixtures import CommandBundleFixture


class BuildLifecycleTests(CommandBundleFixture):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def test_release_debug_release_hits_keep_both_types_and_ignore_jobs(self) -> None:
        """Switching type reuses its own generation without starting build work."""
        debug = self._create_generation("d" * 64, build_type="debug")
        publish_current_bundle(self.output, "phone", debug, build_type="debug")
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(images, "load_container_lock", return_value=self.lock),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
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
            for build_type, jobs in (("release", 1), ("debug", 8), ("release", 8)):
                old = self._create_generation("b" * 64, build_type=build_type)
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    output.run_entrypoint(
                        functools.partial(
                            build_commands.build,
                            "phone",
                            jobs,
                            verbose=True,
                            offline=True,
                            build_type=build_type,
                        )
                    )

                assert (f"build phone --build-type {build_type}: OK (cached)") in (
                    stdout.getvalue()
                )
                expected = self.bundle_path if build_type == "release" else debug
                assert (str(expected.relative_to(self.root))) in (stdout.getvalue())
                assert not (old.exists())
                assert self.bundle_path.is_dir()
                assert debug.is_dir()
            assert (rootfs_gc.call_args_list) == ([mock.call(self.cache)] * 3)
            assert (apks_gc.call_args_list) == ([mock.call(self.cache)] * 3)

    def test_build_result_ignores_closed_stdout_pipe(self) -> None:
        """A closed output consumer must not turn a valid build result into failure."""

        class BrokenPipeStream(io.StringIO):
            def flush(self) -> None:
                raise BrokenPipeError

        with (
            mock.patch.object(common, "ROOT", self.root),
            contextlib.redirect_stdout(BrokenPipeStream()),
        ):
            build_commands._print_build_result(  # noqa: SLF001
                "phone",
                self.bundle,
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
        self, relative: str, mutation: str
    ) -> None:
        """Cache reuse requires every recorded payload byte and file mode to match."""
        identity = bundles_commands.BuildIdentity(
            self.snapshot.recipe, "e" * 64, "a" * 64, self.signing_key
        )
        with mock.patch.object(common, "ROOT", self.root):
            source = self.bundle_path / relative
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

    def test_exact_bundle_identity_and_image_are_a_reusable_hit(self) -> None:
        """Reuse a resolved generation only when its identity and image bytes match."""
        identity = bundles_commands.BuildIdentity(
            self.snapshot.recipe, "e" * 64, "a" * 64, self.signing_key
        )
        with mock.patch.object(common, "ROOT", self.root):
            matched = bundles_commands.matching_target_bundle(
                "phone",
                identity,
                "image/ramboot.bin",
            )

        if matched is None:
            pytest.fail("an exact bundle was not reusable")
        bundle, manifest = matched
        assert (bundle) == (self.bundle)
        assert (manifest) == (json.loads(self.bundle.manifest_bytes))

    @pytest.mark.parametrize(
        ("field", "changed"),
        [
            pytest.param("workspace_digest", "d" * 64, id="workspace"),
            pytest.param("container_image_recipe", "f" * 64, id="image-recipe"),
            pytest.param("container_image_content", "b" * 64, id="image-content"),
            pytest.param("apk_signing_key", "8" * 64, id="signing-key"),
        ],
    )
    def test_each_build_identity_mismatch_is_a_cache_miss(self, field: str, changed: str) -> None:
        """Reject a generation when any host-visible causal identity changed."""
        identity = replace(
            bundles_commands.BuildIdentity("c" * 64, "e" * 64, "a" * 64, self.signing_key),
            **{field: changed},
        )
        with mock.patch.object(common, "ROOT", self.root):
            assert (
                bundles_commands.matching_target_bundle("phone", identity, "image/ramboot.bin")
                is None
            )

    def test_build_miss_requires_host_validation_after_container_success(self) -> None:
        """Container exit zero is insufficient without an exact published generation."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = self._create_generation("b" * 64)
        self._clear_current_bundle()
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(images, "load_container_lock", return_value=self.lock),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(
                image_store,
                "current_image_state",
                return_value=ImageState("e" * 64, "a" * 64, "a" * 64),
            ),
            mock.patch.object(
                image_store,
                "publish_current_image_state",
                return_value=ImageState("e" * 64, "a" * 64, "a" * 64),
            ),
            mock.patch.object(kern_env, "kern_environment", return_value={}),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(workspace_staging, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(prune_operations, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(prune_operations, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True),
            pytest.raises(SystemExit, match="without publishing an exact valid current bundle"),
        ):
            output.run_entrypoint(lambda: build_commands.build("phone", 4))

        assert not (bundle_pointer(self.output, "phone").exists())
        assert old.exists()
        discard.assert_called_once_with(self.snapshot, workspace)
        rootfs_gc.assert_not_called()
        apks_gc.assert_not_called()
        metadata = next((self.root / ".cache/logs/build/phone").rglob("run.json"))
        assert (json.loads(metadata.read_text(encoding="utf-8"))["status"]) == ("failed")

    def test_successful_build_discards_superseded_after_host_validation(self) -> None:
        """Retain old generations until the container result validates on the host."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = self._create_generation("b" * 64)
        self._clear_current_bundle()

        def publish_result(_stage: output.Stage, _command: list[str], **_kwargs: object) -> None:
            publish_current_bundle(self.output, "phone", self.bundle_path)

        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(images, "load_container_lock", return_value=self.lock),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(
                image_store,
                "current_image_state",
                return_value=ImageState("e" * 64, "a" * 64, "a" * 64),
            ),
            mock.patch.object(
                image_store,
                "publish_current_image_state",
                return_value=ImageState("e" * 64, "a" * 64, "a" * 64),
            ),
            mock.patch.object(kern_env, "kern_environment", return_value={}),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(workspace_staging, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(prune_operations, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(prune_operations, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=publish_result),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                output.run_entrypoint(lambda: build_commands.build("phone", 4))

        assert ("build phone --build-type release: OK") in (stdout.getvalue())
        assert ("build phone --build-type release: OK (cached)") not in (stdout.getvalue())
        assert not (old.exists())
        discard.assert_called_once_with(self.snapshot, workspace)
        rootfs_gc.assert_called_once_with(self.cache)
        apks_gc.assert_called_once_with(self.cache)
        metadata = next((self.root / ".cache/logs/build/phone").rglob("run.json"))
        assert (json.loads(metadata.read_text(encoding="utf-8"))["status"]) == ("success")

    def test_offline_build_miss_requires_the_current_image_without_setup(self) -> None:
        """Do not silently rebuild the OCI environment when offline was requested."""
        self._clear_current_bundle()
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release),
            mock.patch.object(
                workspace_inputs,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(images, "load_container_lock", return_value=self.lock),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern_env, "kern_available", return_value=True),
            mock.patch.object(kern_env, "require_kern", return_value="kern"),
            mock.patch.object(image_store, "current_image_state", return_value=None),
            mock.patch.object(
                setup,
                "setup",
                side_effect=AssertionError("offline build must not set up an image"),
            ),
            mock.patch.object(
                workspace_staging,
                "stage_workspace_snapshot",
                side_effect=AssertionError("offline image failure must not stage a workspace"),
            ),
            pytest.raises(SystemExit, match="offline build requires the current pinned OCI image"),
        ):
            output.run_entrypoint(lambda: build_commands.build("phone", 4, offline=True))

        assert not (bundle_pointer(self.output, "phone").exists())

    def test_build_argv_has_explicit_memory_budget_and_narrow_mounts(self) -> None:
        """Request a 2 GiB build budget without exposing broad cache mounts."""
        roots = {
            "workspace": self.root / "workspace",
            "downloads": self.root / "cache/downloads",
            "ccache": self.root / "cache/ccache",
            "host_tools": self.root / "cache/host-tools",
            "apk_signing": self.root / "cache/apk-signing",
            "apks": self.root / "cache/apks",
            "rootfs": self.root / "cache/rootfs",
            "linux": self.root / "cache/linux",
            "output": self.root / "cache/out",
            "logs": self.root / "cache/logs/build/run",
        }
        with mock.patch.dict(os.environ, {}, clear=True):
            command = build_commands._build_container_command(  # noqa: SLF001
                "/usr/bin/kern",
                target="phone",
                jobs=6,
                image="localhost/fplinux-build:locked",
                offline=False,
                snapshot=self.snapshot,
                **roots,
                profile=None,
                build_type="release",
                log_environment={"FPLINUX_LOG_ROOT": "/logs"},
                image_recipe="e" * 64,
                image_content="a" * 64,
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
        assert ("--privileged") in (command)
        assert ("--memory") in (command)
        assert (command[command.index("--memory") + 1]) == ("2g")
        assert not (any(mount.split(":", 2)[1] == "/cache" for mount in mounts))
        assert ("FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE=" + "e" * 64) in (command)
        assert ("FPLINUX_CONTAINER_IMAGE_CONTENT=" + "a" * 64) in (command)
        build = command[command.index("--") + 1 :]
        assert (build[:3]) == (["python3", "-m", "fplinux_cli.build"])
        # The in-container parser accepts these options in any order; compare flag/value pairs.
        assert Counter(zip(build[3::2], build[4::2], strict=True)) == Counter(
            [("--target", "phone"), ("--build-type", "release"), ("--jobs", "6")]
        )
        assert (command[:2]) == (["/usr/bin/kern", "box"])
        network = command.index("--network")
        assert (command[network + 1]) == ("host")

    def test_offline_build_argv_disables_container_network(self) -> None:
        """Offline mode is an execution policy, not a separate build identity."""
        roots = {
            "workspace": self.root / "workspace",
            "downloads": self.root / "cache/downloads",
            "ccache": self.root / "cache/ccache",
            "host_tools": self.root / "cache/host-tools",
            "apk_signing": self.root / "cache/apk-signing",
            "apks": self.root / "cache/apks",
            "rootfs": self.root / "cache/rootfs",
            "linux": self.root / "cache/linux",
            "output": self.root / "cache/out",
            "logs": self.root / "cache/logs/build/run",
        }
        command = build_commands._build_container_command(  # noqa: SLF001
            "/usr/bin/kern",
            target="phone",
            jobs=6,
            image="localhost/fplinux-build:locked",
            offline=True,
            snapshot=self.snapshot,
            **roots,
            profile=None,
            build_type="release",
            log_environment={"FPLINUX_LOG_ROOT": "/logs"},
            image_recipe="e" * 64,
            image_content="a" * 64,
        )

        network = command.index("--network")
        assert (command[network + 1]) == ("none")
