# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for immutable command resolution and exact build orchestration."""

from __future__ import annotations

import contextlib
import functools
import hashlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import commands, output
from fplinux_cli import workspace as workspace_module
from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    bundle_pointer,
    canonical_json_bytes,
    publish_current_bundle,
    published_file_records,
)
from fplinux_cli.config import container_runtime_recipe_digest
from fplinux_cli.image_state import ImageState, publish_image_state
from fplinux_cli.workspace import WorkspaceSnapshot

if TYPE_CHECKING:
    from collections.abc import Callable


class CommandLifecycleTests(unittest.TestCase):
    """Keep cache hits and readers ahead of every mutable or external action."""

    def setUp(self) -> None:
        """Create a published generation and the signing input it claims."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        self.output = self.cache / "out"
        self.target_config: dict[str, object] = {}
        self.release = {"image": "image/ramboot.bin"}
        self.lock = {
            "oci": {
                "repository": "localhost/fplinux-build",
                "platform": "linux/amd64",
            }
        }
        signing_key = self.cache / "apk-signing/fplinux-build.rsa.pub"
        signing_key.parent.mkdir(parents=True)
        signing_key.write_bytes(b"test public signing key\n")
        self.signing_key = hashlib.sha256(signing_key.read_bytes()).hexdigest()
        self.snapshot = WorkspaceSnapshot((), "c" * 64)
        publish_image_state(self.cache, ImageState("e" * 64, "a" * 64))
        self.bundle_path = self._create_generation("a" * 64)
        self.bundle = publish_current_bundle(self.output, "phone", self.bundle_path)

    def _manifest(
        self,
        generation: str,
        path: Path,
        profile: str | None = None,
        *,
        runnable: bool = True,
    ) -> dict[str, object]:
        """Describe one complete synthetic bundle independently of its resolver."""
        return {
            "workspace_digest": self.snapshot.recipe,
            "container_image_recipe": "e" * 64,
            "container_image_generation": "a" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "9" * 64,
            "rootfs_receipt": {"recipe": "f" * 64, "sha256": "0" * 64},
            "boot_artifacts": {"required": [], "runnable": runnable},
            "files": published_file_records(path),
            "generation": generation,
            "kbuild_receipt": {"recipe": "1" * 64, "sha256": "3" * 64},
            "linux_recipe": "2" * 64,
            "profile": profile,
            "target": "phone",
        }

    def _create_generation(
        self,
        generation: str,
        image: bytes = b"ramboot\n",
        *,
        profile: str | None = None,
        runnable: bool = True,
    ) -> Path:
        """Write one complete immutable generation without selecting it."""
        slot = self.output / "phone"
        if profile is not None:
            slot = slot / "profiles" / profile
        path = slot / "bundles" / generation
        path.mkdir(parents=True)
        payload = path / self.release["image"]
        payload.parent.mkdir(parents=True)
        payload.write_bytes(image)
        payload.chmod(0o644)
        runner = path / "runner/run.py"
        runner.parent.mkdir()
        runner.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
        runner.chmod(0o755)
        client = path / "host/fplinux-usb-keyboard"
        client.parent.mkdir()
        client.write_text("keyboard client\n", encoding="utf-8")
        client.chmod(0o755)
        ssh_helper = path / "runner/ssh_transport.py"
        ssh_helper.write_text("# bundled SSH helper\n", encoding="utf-8")
        ssh_helper.chmod(0o644)
        (path / BUILD_MANIFEST_NAME).write_bytes(
            canonical_json_bytes(self._manifest(generation, path, profile, runnable=runnable))
        )
        return path

    def _clear_current_bundle(self) -> None:
        """Leave complete generations present while making the current receipt miss."""
        bundle_pointer(self.output, "phone").unlink()

    def test_exact_build_hit_ignores_jobs_and_avoids_runtime_or_staging(self) -> None:
        """Both job counts reuse the same valid generation without starting build work."""
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=self.target_config),
            mock.patch.object(commands, "load_release", return_value=self.release),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "load_container_lock", return_value=self.lock),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                commands,
                "kern_available",
                side_effect=AssertionError("cache hit must not inspect Kern"),
            ),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                side_effect=AssertionError("cache hit must not stage a workspace"),
            ),
            mock.patch.object(
                commands,
                "discard_staged_workspace_snapshot",
                side_effect=AssertionError("cache hit must not discard an unstaged workspace"),
            ),
            mock.patch.object(commands, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(commands, "discard_obsolete_apks") as apks_gc,
        ):
            for jobs in (1, 8):
                with self.subTest(jobs=jobs):
                    old = self._create_generation("b" * 64)
                    stdout = io.StringIO()
                    with contextlib.redirect_stdout(stdout):
                        output.run_entrypoint(
                            functools.partial(
                                commands.build,
                                "phone",
                                jobs,
                                verbose=True,
                                offline=True,
                            )
                        )

                    self.assertIn("build phone: OK (cached)", stdout.getvalue())
                    self.assertFalse(old.exists())
            self.assertEqual(
                rootfs_gc.call_args_list,
                [mock.call(self.cache), mock.call(self.cache)],
            )
            self.assertEqual(
                apks_gc.call_args_list,
                [mock.call(self.cache), mock.call(self.cache)],
            )

    def test_build_result_ignores_closed_stdout_pipe(self) -> None:
        """A closed output consumer must not turn a valid build result into failure."""

        class BrokenPipeStream(io.StringIO):
            def flush(self) -> None:
                raise BrokenPipeError

        with (
            mock.patch.object(commands, "ROOT", self.root),
            contextlib.redirect_stdout(BrokenPipeStream()),
        ):
            commands._print_build_result(  # noqa: SLF001
                "phone",
                self.bundle,
                {"image": "image/ramboot.bin"},
                cached=False,
            )

    def test_corrupted_bundle_image_is_not_an_exact_hit(self) -> None:
        """A bundle whose image bytes drifted from the manifest is rebuilt."""
        (self.bundle_path / "image/ramboot.bin").write_bytes(b"corrupt\n")
        identity = commands.BuildIdentity(
            self.snapshot.recipe, "e" * 64, "a" * 64, self.signing_key
        )
        with mock.patch.object(commands, "ROOT", self.root):
            self.assertIsNone(
                commands._matching_target_bundle(  # noqa: SLF001
                    "phone",
                    identity,
                    "image/ramboot.bin",
                )
            )

    def test_exact_bundle_identity_and_image_are_a_reusable_hit(self) -> None:
        """Reuse a resolved generation only when its identity and image bytes match."""
        identity = commands.BuildIdentity(
            self.snapshot.recipe, "e" * 64, "a" * 64, self.signing_key
        )
        with mock.patch.object(commands, "ROOT", self.root):
            matched = commands._matching_target_bundle(  # noqa: SLF001
                "phone",
                identity,
                "image/ramboot.bin",
            )

        if matched is None:
            self.fail("an exact bundle was not reusable")
        bundle, manifest = matched
        self.assertEqual(bundle, self.bundle)
        self.assertEqual(manifest, json.loads(self.bundle.manifest_bytes))

    def test_each_build_identity_mismatch_is_a_cache_miss(self) -> None:
        """Reject a generation when any host-visible causal identity changed."""
        mismatches = (
            commands.BuildIdentity("d" * 64, "e" * 64, "a" * 64, self.signing_key),
            commands.BuildIdentity("c" * 64, "f" * 64, "a" * 64, self.signing_key),
            commands.BuildIdentity("c" * 64, "e" * 64, "b" * 64, self.signing_key),
            commands.BuildIdentity("c" * 64, "e" * 64, "a" * 64, "8" * 64),
        )
        with mock.patch.object(commands, "ROOT", self.root):
            for identity in mismatches:
                with self.subTest(identity=identity):
                    self.assertIsNone(
                        commands._matching_target_bundle(  # noqa: SLF001
                            "phone",
                            identity,
                            "image/ramboot.bin",
                        )
                    )

    def test_build_miss_requires_host_validation_after_container_success(self) -> None:
        """Container exit zero is insufficient without an exact published generation."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = self._create_generation("b" * 64)
        self._clear_current_bundle()
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=self.target_config),
            mock.patch.object(commands, "load_release", return_value=self.release),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "load_container_lock", return_value=self.lock),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(commands, "kern_available", return_value=True),
            mock.patch.object(commands, "require_kern", return_value="kern"),
            mock.patch.object(
                commands,
                "current_image_state",
                return_value=ImageState("e" * 64, "a" * 64),
            ),
            mock.patch.object(
                commands,
                "publish_current_image_state",
                return_value=ImageState("e" * 64, "a" * 64),
            ),
            mock.patch.object(commands, "kern_environment", return_value={}),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(commands, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(commands, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(commands, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True),
            self.assertRaisesRegex(
                SystemExit,
                "without publishing an exact valid current bundle",
            ),
        ):
            output.run_entrypoint(lambda: commands.build("phone", 4))

        self.assertFalse(bundle_pointer(self.output, "phone").exists())
        self.assertTrue(old.exists())
        discard.assert_called_once_with(self.snapshot, workspace)
        rootfs_gc.assert_not_called()
        apks_gc.assert_not_called()
        metadata = next((self.root / ".cache/logs/build/phone").rglob("run.json"))
        self.assertEqual(json.loads(metadata.read_text(encoding="utf-8"))["status"], "failed")

    def test_successful_build_discards_superseded_after_host_validation(self) -> None:
        """Retain old generations until the container result validates on the host."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = self._create_generation("b" * 64)
        self._clear_current_bundle()

        def publish_result(_stage: output.Stage, _command: list[str], **_kwargs: object) -> None:
            publish_current_bundle(self.output, "phone", self.bundle_path)

        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=self.target_config),
            mock.patch.object(commands, "load_release", return_value=self.release),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "load_container_lock", return_value=self.lock),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(commands, "kern_available", return_value=True),
            mock.patch.object(commands, "require_kern", return_value="kern"),
            mock.patch.object(
                commands,
                "current_image_state",
                return_value=ImageState("e" * 64, "a" * 64),
            ),
            mock.patch.object(
                commands,
                "publish_current_image_state",
                return_value=ImageState("e" * 64, "a" * 64),
            ),
            mock.patch.object(commands, "kern_environment", return_value={}),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(commands, "discard_staged_workspace_snapshot") as discard,
            mock.patch.object(commands, "discard_obsolete_rootfs") as rootfs_gc,
            mock.patch.object(commands, "discard_obsolete_apks") as apks_gc,
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=publish_result),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                output.run_entrypoint(lambda: commands.build("phone", 4))

        self.assertIn("build phone: OK", stdout.getvalue())
        self.assertNotIn("build phone: OK (cached)", stdout.getvalue())
        self.assertFalse(old.exists())
        discard.assert_called_once_with(self.snapshot, workspace)
        rootfs_gc.assert_called_once_with(self.cache)
        apks_gc.assert_called_once_with(self.cache)
        metadata = next((self.root / ".cache/logs/build/phone").rglob("run.json"))
        self.assertEqual(json.loads(metadata.read_text(encoding="utf-8"))["status"], "success")

    def test_offline_build_miss_requires_the_current_image_without_setup(self) -> None:
        """Do not silently rebuild the OCI environment when offline was requested."""
        self._clear_current_bundle()
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=self.target_config),
            mock.patch.object(commands, "load_release", return_value=self.release),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "load_container_lock", return_value=self.lock),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(commands, "kern_available", return_value=True),
            mock.patch.object(commands, "require_kern", return_value="kern"),
            mock.patch.object(commands, "current_image_state", return_value=None),
            mock.patch.object(
                commands,
                "setup",
                side_effect=AssertionError("offline build must not set up an image"),
            ),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                side_effect=AssertionError("offline image failure must not stage a workspace"),
            ),
            self.assertRaisesRegex(
                SystemExit,
                "offline build requires the current pinned OCI image",
            ),
        ):
            output.run_entrypoint(lambda: commands.build("phone", 4, offline=True))

        self.assertFalse(bundle_pointer(self.output, "phone").exists())

    def test_run_executes_a_runner_from_the_resolved_generation(self) -> None:
        """Resolve current once and preserve that immutable generation path."""
        runner = self.bundle_path / "runner/run.py"
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value={}),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.run_target("phone")

        execute.assert_called_once_with(os.fsencode(runner), [os.fsencode(runner)])

    def test_microsd_context_selection_has_no_fallback(self) -> None:
        """Resolve the Nokia microSD context without making the profile an alias."""
        self.assertEqual(
            commands.selected_context_profile(
                "nokia-ta1618",
                profile=None,
                boot="microsd",
            ),
            "microsd-uboot",
        )
        self.assertIsNone(
            commands.selected_context_profile("nokia-ta1618", profile=None, boot=None)
        )
        self.assertEqual(
            commands.selected_context_profile(
                "nokia-ta1618",
                profile="microsd-uboot",
                boot=None,
            ),
            "microsd-uboot",
        )
        with self.assertRaisesRegex(SystemExit, "not available for target inoi-240-modern-4g"):
            commands.selected_context_profile(
                "inoi-240-modern-4g",
                profile=None,
                boot="microsd",
            )
        with self.assertRaisesRegex(SystemExit, "cannot be used together"):
            commands.selected_context_profile(
                "nokia-ta1618",
                profile="microsd-uboot",
                boot="microsd",
            )

    def test_microsd_context_opens_the_selected_profile_runner(self) -> None:
        """The microSD context consumes the existing profile generation exactly once."""
        profile = "microsd-uboot"
        profile_path = self._create_generation("b" * 64, profile=profile)
        profile_bundle = publish_current_bundle(
            self.output,
            "phone",
            profile_path,
            profile,
        )
        manifest = self._manifest("b" * 64, profile_bundle.path, profile)
        with (
            mock.patch.object(
                commands,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ) as load_target,
            mock.patch.object(
                commands,
                "_resolve_target_bundle",
                return_value=(profile_bundle, manifest),
            ) as resolve,
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.run_target("nokia-ta1618", boot="microsd")

        load_target.assert_called_once_with("nokia-ta1618", profile)
        resolve.assert_called_once_with("nokia-ta1618", profile)
        runner = profile_bundle.path / "runner/run.py"
        execute.assert_called_once_with(os.fsencode(runner), [os.fsencode(runner)])

    def test_run_profile_executes_only_that_profiles_current_generation(self) -> None:
        """A named run does not fall back to the target's default bundle pointer."""
        profile = "usb-host-lab"
        profile_path = self._create_generation("b" * 64, profile=profile)
        profile_bundle = publish_current_bundle(
            self.output,
            "phone",
            profile_path,
            profile,
        )
        runner = profile_bundle.path / "runner/run.py"

        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(
                commands,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.run_target("phone", profile=profile)

        execute.assert_called_once_with(os.fsencode(runner), [os.fsencode(runner)])

    def test_build_only_profile_is_rejected_before_bundle_or_usb_access(self) -> None:
        """A profile without a complete boot path cannot start the RAM loader."""
        with (
            mock.patch.object(
                commands,
                "load_target",
                return_value={"runtime": {"runnable": False}},
            ),
            mock.patch.object(
                commands,
                "_resolve_target_bundle",
                side_effect=AssertionError("build-only profile must not resolve a bundle"),
            ),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
            self.assertRaisesRegex(SystemExit, "profile is build-only"),
        ):
            commands.run_target("phone", profile="microsd")

        execute.assert_not_called()

    def test_stale_build_only_profile_bundle_remains_non_runnable(self) -> None:
        """Changing source policy cannot authorize an older build-only bundle."""
        profile = "microsd"
        profile_path = self._create_generation("c" * 64, profile=profile, runnable=False)
        publish_current_bundle(self.output, "phone", profile_path, profile)
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(
                commands,
                "load_target",
                return_value={"runtime": {"runnable": True}},
            ),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
            self.assertRaisesRegex(SystemExit, "profile bundle is build-only"),
        ):
            commands.run_target("phone", profile=profile)

        execute.assert_not_called()

    def test_verify_propagates_the_authenticated_runtime_identity_failure(self) -> None:
        """Do not report success when reconnect cannot identify the running kernel."""
        target_config: dict[str, object] = {}
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(
                commands,
                "container_image_recipe_digest",
                return_value="e" * 64,
            ),
            mock.patch.object(
                commands,
                "_current_ssh_session",
                side_effect=SystemExit(
                    "SSH transport failed: cannot read the running kernel identity (exit 7)"
                ),
            ),
            self.assertRaisesRegex(SystemExit, r"running kernel identity \(exit 7\)"),
        ):
            commands.verify_booted("phone")

    def test_reconnect_accepts_an_older_session_generation_for_the_same_device_runtime(
        self,
    ) -> None:
        """An application-only bundle rebuild does not invalidate the loaded kernel."""
        manifest = json.loads(self.bundle.manifest_bytes)
        session: dict[str, str] = {}
        ssh = mock.Mock()
        ssh.load_bundle_context.return_value = (
            {"target": "phone"},
            {"bundle_generation": self.bundle.generation},
        )
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = session
        ssh.require_device_identity.return_value = "6.12-fplinux-9999999999999999"

        with mock.patch.object(commands, "_load_bundle_ssh_helper", return_value=ssh):
            resolved_ssh, resolved_session = commands._current_ssh_session(  # noqa: SLF001
                self.bundle,
                manifest,
                "phone",
            )

        self.assertIs(resolved_ssh, ssh)
        self.assertIs(resolved_session, session)
        ssh.load_current_session.assert_called_once_with("phone")
        ssh.reacquire_bound_session.assert_called_once_with(session)
        ssh.require_device_identity.assert_called_once_with(session, "9" * 64)

    def test_reconnect_rejects_an_authenticated_session_with_another_device_runtime(
        self,
    ) -> None:
        """Do not use a reconnected phone whose kernel differs from the selected bundle."""
        manifest = json.loads(self.bundle.manifest_bytes)
        session: dict[str, str] = {}
        ssh = mock.Mock()
        ssh.load_bundle_context.return_value = (
            {"target": "phone"},
            {"bundle_generation": self.bundle.generation},
        )
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = session
        ssh.require_device_identity.side_effect = SystemExit(
            "SSH transport failed: current SSH session exposes a different kernel identity"
        )

        with (
            mock.patch.object(commands, "_load_bundle_ssh_helper", return_value=ssh),
            self.assertRaisesRegex(SystemExit, "different kernel identity"),
        ):
            commands._current_ssh_session(self.bundle, manifest, "phone")  # noqa: SLF001

        ssh.require_device_identity.assert_called_once_with(session, "9" * 64)

    def test_verify_reports_the_manifest_identity_after_authenticated_reconnect(self) -> None:
        """Report the selected device identity after the reconnect boundary accepts it."""
        target_config: dict[str, object] = {}
        stdout = io.StringIO()
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(commands, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(
                commands,
                "_current_ssh_session",
                return_value=(mock.Mock(), {}),
            ) as current_session,
            contextlib.redirect_stdout(stdout),
        ):
            commands.verify_booted("phone")

        self.assertEqual(
            stdout.getvalue(),
            "verify: the phone runs the current build (9999999999999999)\n",
        )
        current_session.assert_called_once_with(self.bundle, mock.ANY, "phone")

    def test_console_uses_ssh_for_commands_and_keyboard_tool_for_evdev(self) -> None:
        """Route commands through SSH and evdev through the keyboard client."""
        target_config = {
            "runtime": {
                "usb": {
                    "linux_gadget": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                        "keyboard_interface": 1,
                    },
                },
            },
        }
        result = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        ssh = mock.Mock(run_remote=mock.Mock(return_value=result))
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch.object(commands, "_current_ssh_session", return_value=(ssh, {})),
        ):
            commands.console_target(
                "phone",
                keyboard=None,
                exec_command="id",
                upload=None,
                pull=None,
            )
        ssh.run_remote.assert_called_once_with({}, "id")

        client = self.bundle_path / "host/fplinux-usb-keyboard"
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=target_config),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.console_target(
                "phone",
                keyboard="UP",
                exec_command=None,
                upload=None,
                pull=None,
            )
        execute.assert_called_once_with(
            client,
            [
                str(client),
                "--vid",
                "1782",
                "--pid",
                "4d00",
                "--wait",
                "10",
                "--interface",
                "1",
                "--keyboard",
                "UP",
            ],
        )

    def test_console_profile_reconnects_only_through_its_selected_generation(self) -> None:
        """A profile RAM session is never compared with the target's default bundle."""
        profile = "microsd-uboot"
        profile_path = self._create_generation("b" * 64, profile=profile)
        profile_bundle = publish_current_bundle(
            self.output,
            "phone",
            profile_path,
            profile,
        )
        target_config = {
            "runtime": {
                "usb": {
                    "linux_gadget": {
                        "vendor_id": 0x0525,
                        "product_id": 0xA4A6,
                        "wait_seconds": 60,
                        "keyboard_interface": 1,
                    },
                },
            },
        }
        result = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        ssh = mock.Mock(run_remote=mock.Mock(return_value=result))
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value=target_config) as load_target,
            mock.patch.object(
                commands,
                "_current_ssh_session",
                return_value=(ssh, {}),
            ) as current_session,
        ):
            commands.console_target(
                "phone",
                profile=profile,
                keyboard=None,
                exec_command="id",
                upload=None,
                pull=None,
            )

        load_target.assert_called_once_with("phone", profile)
        selected_bundle, selected_manifest, selected_target = current_session.call_args.args
        self.assertEqual(selected_bundle, profile_bundle)
        self.assertEqual(selected_manifest["profile"], profile)
        self.assertEqual(selected_target, "phone")
        ssh.run_remote.assert_called_once_with({}, "id")

    def test_build_argv_has_only_exact_nonoverlapping_mount_roots(self) -> None:
        """Do not expose the cache root or an ancestor alias to the build container."""
        roots = {
            "workspace": self.root / "workspace",
            "downloads": self.root / "cache/downloads",
            "apk_signing": self.root / "cache/apk-signing",
            "apks": self.root / "cache/apks",
            "rootfs": self.root / "cache/rootfs",
            "linux": self.root / "cache/linux",
            "output": self.root / "cache/out",
            "logs": self.root / "cache/logs/build/run",
        }
        with mock.patch.dict(os.environ, {}, clear=True):
            command = commands._build_container_command(  # noqa: SLF001
                "/usr/bin/kern",
                target="phone",
                jobs=6,
                image="localhost/fplinux-build:locked",
                offline=False,
                snapshot=self.snapshot,
                **roots,
                profile=None,
                log_environment={"FPLINUX_LOG_ROOT": "/logs"},
                image_recipe="e" * 64,
                image_generation="a" * 64,
            )

        mounts = [command[index + 1] for index, value in enumerate(command) if value == "--volume"]
        self.assertEqual(
            mounts,
            [
                f"{roots['downloads']}:/cache/downloads",
                f"{roots['apk_signing']}:/cache/apk-signing",
                f"{roots['apks']}:/cache/apks",
                f"{roots['rootfs']}:/cache/rootfs",
                f"{roots['linux']}:/cache/linux",
                f"{roots['output']}:/out",
                f"{roots['logs']}:/logs",
                f"{roots['workspace']}:/workspace:ro",
            ],
        )
        self.assertIn("--read-only", command)
        self.assertIn("--privileged", command)
        self.assertFalse(any(mount.split(":", 2)[1] == "/cache" for mount in mounts))
        self.assertIn(
            "FPLINUX_CONTAINER_IMAGE_RECIPE="
            + container_runtime_recipe_digest("e" * 64, "a" * 64),
            command,
        )
        self.assertIn("FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE=" + "e" * 64, command)
        self.assertIn("FPLINUX_CONTAINER_IMAGE_GENERATION=" + "a" * 64, command)
        self.assertEqual(
            command[-8:],
            [
                "--",
                "python3",
                "-m",
                "fplinux_cli.builder",
                "--target",
                "phone",
                "--jobs",
                "6",
            ],
        )
        self.assertEqual(command[:2], ["/usr/bin/kern", "box"])
        network = command.index("--network")
        self.assertEqual(command[network + 1], "host")

    def test_offline_build_argv_disables_container_network(self) -> None:
        """Offline mode is an execution policy, not a separate build identity."""
        roots = {
            "workspace": self.root / "workspace",
            "downloads": self.root / "cache/downloads",
            "apk_signing": self.root / "cache/apk-signing",
            "apks": self.root / "cache/apks",
            "rootfs": self.root / "cache/rootfs",
            "linux": self.root / "cache/linux",
            "output": self.root / "cache/out",
            "logs": self.root / "cache/logs/build/run",
        }
        command = commands._build_container_command(  # noqa: SLF001
            "/usr/bin/kern",
            target="phone",
            jobs=6,
            image="localhost/fplinux-build:locked",
            offline=True,
            snapshot=self.snapshot,
            **roots,
            profile=None,
            log_environment={"FPLINUX_LOG_ROOT": "/logs"},
            image_recipe="e" * 64,
            image_generation="a" * 64,
        )

        network = command.index("--network")
        self.assertEqual(command[network + 1], "none")


class ChecksumAportTests(unittest.TestCase):
    """Publish only validated checksum-block changes from an isolated OCI stage."""

    PACKAGE = "synthetic-checksum-aport"
    BEFORE_APKBUILD = (
        b"pkgname=synthetic-checksum-aport\n"
        b"pkgver=1\n"
        b'source="local.c"\n'
        b'sha512sums="\n'
        b"old-local  local.c\n"
        b'"\n'
        b"package() {\n"
        b"\t:\n"
        b"}\n"
    )
    AFTER_APKBUILD = BEFORE_APKBUILD.replace(
        b'sha512sums="\nold-local  local.c\n"\n',
        b'sha512sums="\nnew-local  local.c\n"\n',
    )

    def setUp(self) -> None:
        """Create one canonical aport with an ordinary local source."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.aport = self.root / "alpine/aports" / self.PACKAGE
        self.aport.mkdir(parents=True)
        self.apkbuild = self.aport / "APKBUILD"
        self.apkbuild.write_bytes(self.BEFORE_APKBUILD)
        self.apkbuild.chmod(0o640)
        (self.aport / "local.c").write_bytes(b"int local_source;\n")

    def _run(self, container_run: Callable[..., None]) -> None:
        """Invoke the real checksum workflow with only its OCI execution replaced."""
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(workspace_module, "ROOT", self.root),
            mock.patch.object(output, "ROOT", self.root),
            mock.patch.object(commands, "kern_available", return_value=True),
            mock.patch.object(commands, "require_kern", return_value="/usr/bin/kern"),
            mock.patch.object(
                commands,
                "current_image_state",
                return_value=ImageState("e" * 64, "a" * 64),
            ),
            mock.patch.object(commands, "kern_environment", return_value={}),
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=container_run),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            output.run_entrypoint(lambda: commands.checksum_aport(self.PACKAGE, offline=True))

    def _staged_apkbuild(self, command: list[str]) -> Path:
        """Resolve the private aport through the OCI workspace mount boundary."""
        workspace_sources: list[Path] = []
        for index, argument in enumerate(command[:-1]):
            if argument != "--volume":
                continue
            mount = command[index + 1].split(":")
            if len(mount) >= 2 and mount[1] == "/workspace":
                workspace_sources.append(Path(mount[0]))
        self.assertEqual(len(workspace_sources), 1)

        workdir_index = command.index("--workdir")
        container_workdir = Path(command[workdir_index + 1])
        relative_workdir = container_workdir.relative_to("/workspace")
        return workspace_sources[0] / relative_workdir / "APKBUILD"

    def test_success_replaces_only_the_checksum_block_atomically(self) -> None:
        """Publish complete generated bytes while preserving recipe text and mode."""
        inode_before = self.apkbuild.stat().st_ino

        def generate(_stage: output.Stage, command: list[str], **_kwargs: object) -> None:
            generated = self._staged_apkbuild(command)
            generated.write_bytes(self.AFTER_APKBUILD)

        self._run(generate)

        self.assertEqual(self.apkbuild.read_bytes(), self.AFTER_APKBUILD)
        self.assertEqual(self.apkbuild.stat().st_mode & 0o777, 0o640)
        self.assertNotEqual(self.apkbuild.stat().st_ino, inode_before)
        self.assertEqual(list(self.aport.glob(".APKBUILD.*")), [])

    def test_container_shares_downloads_and_keeps_the_stage_private(self) -> None:
        """Keep the persistent source cache shared without exposing the private stage."""

        def inspect(_stage: output.Stage, command: list[str], **_kwargs: object) -> None:
            mounts = {}
            for index, argument in enumerate(command[:-1]):
                if argument != "--volume":
                    continue
                parts = command[index + 1].split(":")
                destination = parts[1]
                mounts[destination] = "ro" if parts[2:] == ["ro"] else "rw"
            self.assertEqual(
                mounts,
                {
                    "/cache/downloads": "rw",
                    "/workspace": "rw",
                },
            )
            generated = self._staged_apkbuild(command)
            generated.write_bytes(self.AFTER_APKBUILD)

        self._run(inspect)

    def test_non_checksum_generation_failure_keeps_canonical_apkbuild(self) -> None:
        """Reject OCI output that changes recipe text before atomic publication."""
        inode_before = self.apkbuild.stat().st_ino

        def generate_invalid(_stage: output.Stage, command: list[str], **_kwargs: object) -> None:
            generated = self._staged_apkbuild(command)
            generated.write_bytes(self.AFTER_APKBUILD.replace(b"pkgver=1\n", b"pkgver=2\n"))

        with self.assertRaises(SystemExit):
            self._run(generate_invalid)

        self.assertEqual(self.apkbuild.read_bytes(), self.BEFORE_APKBUILD)
        self.assertEqual(self.apkbuild.stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.apkbuild.stat().st_ino, inode_before)
        self.assertEqual(list(self.aport.glob(".APKBUILD.*")), [])


if __name__ == "__main__":
    unittest.main()
