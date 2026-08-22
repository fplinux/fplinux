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
        self.profile = "default"
        self.target_config = {"profile": self.profile}
        self.release = {"image": "image/ramboot.bin"}
        self.lock = {
            "oci": {
                "image": "localhost/fplinux:locked",
                "platform": "linux/amd64",
            }
        }
        signing_key = self.cache / "apk-signing/fplinux-build.rsa.pub"
        signing_key.parent.mkdir(parents=True)
        signing_key.write_bytes(b"test public signing key\n")
        self.signing_key = hashlib.sha256(signing_key.read_bytes()).hexdigest()
        self.snapshot = WorkspaceSnapshot((), "c" * 64)
        self.bundle_path = self._create_generation("a" * 64)
        self.bundle = publish_current_bundle(self.output, "phone", self.profile, self.bundle_path)

    def _manifest(self, generation: str, path: Path) -> dict[str, object]:
        """Describe one complete synthetic bundle independently of its resolver."""
        return {
            "workspace_digest": self.snapshot.recipe,
            "container_image_recipe": "e" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "9" * 64,
            "rootfs_receipt": {"recipe": "f" * 64, "sha256": "0" * 64},
            "files": published_file_records(path),
            "generation": generation,
            "kbuild_receipt": {"recipe": "1" * 64, "sha256": "3" * 64},
            "linux_recipe": "2" * 64,
            "profile": self.profile,
            "target": "phone",
        }

    def _create_generation(self, generation: str, image: bytes = b"ramboot\n") -> Path:
        """Write one complete immutable generation without selecting it."""
        path = self.output / "phone/bundles" / self.profile / generation
        path.mkdir(parents=True)
        payload = path / self.release["image"]
        payload.parent.mkdir(parents=True)
        payload.write_bytes(image)
        payload.chmod(0o644)
        runner = path / "runner/run.py"
        runner.parent.mkdir()
        runner.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
        runner.chmod(0o755)
        client = path / "host/fplinux-usb-console"
        client.parent.mkdir()
        client.write_text("console client\n", encoding="utf-8")
        client.chmod(0o755)
        (path / BUILD_MANIFEST_NAME).write_bytes(
            canonical_json_bytes(self._manifest(generation, path))
        )
        return path

    def _clear_current_bundle(self) -> None:
        """Leave complete generations present while making the current receipt miss."""
        bundle_pointer(self.output, "phone", self.profile).unlink()

    def test_exact_build_hit_ignores_jobs_and_avoids_podman_or_staging(self) -> None:
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
                "require_podman",
                side_effect=AssertionError("cache hit must not inspect Podman"),
            ),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                side_effect=AssertionError("cache hit must not stage a workspace"),
            ),
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
        identity = commands.BuildIdentity(self.snapshot.recipe, "e" * 64, self.signing_key)
        with mock.patch.object(commands, "ROOT", self.root):
            self.assertIsNone(
                commands._matching_target_bundle(  # noqa: SLF001
                    "phone",
                    {"profile": "default"},
                    identity,
                    "image/ramboot.bin",
                )
            )

    def test_exact_bundle_identity_and_image_are_a_reusable_hit(self) -> None:
        """Reuse a resolved generation only when its identity and image bytes match."""
        identity = commands.BuildIdentity(self.snapshot.recipe, "e" * 64, self.signing_key)
        with mock.patch.object(commands, "ROOT", self.root):
            matched = commands._matching_target_bundle(  # noqa: SLF001
                "phone",
                {"profile": "default"},
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
            commands.BuildIdentity("d" * 64, "e" * 64, self.signing_key),
            commands.BuildIdentity("c" * 64, "f" * 64, self.signing_key),
            commands.BuildIdentity("c" * 64, "e" * 64, "8" * 64),
        )
        with mock.patch.object(commands, "ROOT", self.root):
            for identity in mismatches:
                with self.subTest(identity=identity):
                    self.assertIsNone(
                        commands._matching_target_bundle(  # noqa: SLF001
                            "phone",
                            {"profile": "default"},
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
            mock.patch.object(commands, "require_podman", return_value="podman"),
            mock.patch.object(commands, "image_ready", return_value=True),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(output.Stage, "run", autospec=True),
            self.assertRaisesRegex(
                SystemExit,
                "without publishing an exact valid current bundle",
            ),
        ):
            output.run_entrypoint(lambda: commands.build("phone", 4))

        self.assertFalse(bundle_pointer(self.output, "phone", self.profile).exists())
        self.assertTrue(old.exists())
        metadata = next((self.root / ".cache/logs/build/phone").rglob("run.json"))
        self.assertEqual(json.loads(metadata.read_text(encoding="utf-8"))["status"], "failed")

    def test_successful_build_discards_superseded_after_host_validation(self) -> None:
        """Retain old generations until the container result validates on the host."""
        workspace = self.root / ".cache/workspaces/current"
        workspace.mkdir(parents=True)
        old = self._create_generation("b" * 64)
        self._clear_current_bundle()

        def publish_result(_stage: output.Stage, _command: list[str]) -> None:
            publish_current_bundle(self.output, "phone", self.profile, self.bundle_path)

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
            mock.patch.object(commands, "require_podman", return_value="podman"),
            mock.patch.object(commands, "image_ready", return_value=True),
            mock.patch.object(
                commands,
                "stage_workspace_snapshot",
                return_value=workspace,
            ),
            mock.patch.object(output.Stage, "run", autospec=True, side_effect=publish_result),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                output.run_entrypoint(lambda: commands.build("phone", 4))

        self.assertIn("build phone: OK", stdout.getvalue())
        self.assertNotIn("build phone: OK (cached)", stdout.getvalue())
        self.assertFalse(old.exists())
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
            mock.patch.object(commands, "require_podman", return_value="podman"),
            mock.patch.object(commands, "image_ready", return_value=False),
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

        self.assertFalse(bundle_pointer(self.output, "phone", self.profile).exists())

    def test_run_executes_a_runner_from_the_resolved_generation(self) -> None:
        """Resolve current once and preserve that immutable generation path."""
        runner = self.bundle_path / "runner/run.py"
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_target", return_value={"profile": "default"}),
            mock.patch("fplinux_cli.commands.os.execv") as execute,
        ):
            commands.run_target("phone")

        execute.assert_called_once_with(os.fsencode(runner), [os.fsencode(runner)])

    def test_verify_rejects_nonzero_console_status_even_with_matching_stdout(self) -> None:
        """Reject a mocked failed console result even when its stdout happens to match."""
        client = self.bundle_path / "host/fplinux-usb-console"
        target_config = {
            "profile": "default",
            "runtime": {
                "usb": {
                    "linux_console": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                    }
                }
            },
        }
        result = subprocess.CompletedProcess(
            [],
            7,
            stdout=f"6.12-fplinux-{'9' * 16}\n",
            stderr="transport failed\n",
        )
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
            mock.patch("fplinux_cli.commands.subprocess.run", return_value=result) as console_run,
            self.assertRaisesRegex(SystemExit, "console client failed with exit status 7"),
        ):
            commands.verify_booted("phone")
        console_run.assert_called_once_with(
            [
                str(client),
                "--vid",
                "1782",
                "--pid",
                "4d00",
                "--wait",
                "10",
                "--interface",
                "0",
                "--exec",
                "uname -r",
            ],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_verify_matches_the_device_identity_not_the_workspace_digest(self) -> None:
        """Interpret a mocked uname result using the bundle's device-identity suffix."""
        client = self.bundle_path / "host/fplinux-usb-console"
        target_config = {
            "profile": "default",
            "runtime": {
                "usb": {
                    "linux_console": {
                        "vendor_id": 0x1782,
                        "product_id": 0x4D00,
                        "wait_seconds": 10,
                    }
                }
            },
        }
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout=f"6.12-fplinux-{'9' * 16}\n",
            stderr="",
        )
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
            mock.patch("fplinux_cli.commands.subprocess.run", return_value=result) as console_run,
            contextlib.redirect_stdout(stdout),
        ):
            commands.verify_booted("phone")

        self.assertEqual(
            stdout.getvalue(),
            "verify: the phone runs the current build (9999999999999999)\n",
        )
        console_run.assert_called_once_with(
            [
                str(client),
                "--vid",
                "1782",
                "--pid",
                "4d00",
                "--wait",
                "10",
                "--interface",
                "0",
                "--exec",
                "uname -r",
            ],
            capture_output=True,
            text=True,
            check=False,
        )

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
                "/usr/bin/podman",
                target="phone",
                jobs=6,
                platform="linux/amd64",
                image="localhost/fplinux-build:locked",
                offline=False,
                snapshot=self.snapshot,
                **roots,
                log_environment={"FPLINUX_LOG_ROOT": "/logs"},
                image_recipe="e" * 64,
            )

        mounts = [command[index + 1] for index, value in enumerate(command) if value == "--volume"]
        self.assertEqual(
            mounts,
            [
                f"{roots['downloads']}:/cache/downloads:rw,Z",
                f"{roots['apk_signing']}:/cache/apk-signing:rw,Z",
                f"{roots['apks']}:/cache/apks:rw,Z",
                f"{roots['rootfs']}:/cache/rootfs:rw,Z",
                f"{roots['linux']}:/cache/linux:rw,Z",
                f"{roots['output']}:/out:rw,Z",
                f"{roots['logs']}:/logs:rw,Z",
                f"{roots['workspace']}:/workspace:ro,Z",
            ],
        )
        self.assertNotIn(f"{self.root / 'cache'}:/cache:rw,Z", command)
        self.assertIn("--read-only", command)
        self.assertFalse(any(mount.split(":", 2)[1] == "/cache" for mount in mounts))
        self.assertEqual(
            command[-8:],
            [
                "localhost/fplinux-build:locked",
                "python3",
                "-m",
                "fplinux_cli.builder",
                "--target",
                "phone",
                "--jobs",
                "6",
            ],
        )
        self.assertEqual(
            command[:2],
            ["/usr/bin/podman", "run"],
        )
        self.assertNotIn("--network=none", command)

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
            "/usr/bin/podman",
            target="phone",
            jobs=6,
            platform="linux/amd64",
            image="localhost/fplinux-build:locked",
            offline=True,
            snapshot=self.snapshot,
            **roots,
            log_environment={"FPLINUX_LOG_ROOT": "/logs"},
            image_recipe="e" * 64,
        )

        self.assertIn("--network=none", command)
        self.assertLess(command.index("--network=none"), command.index("--platform"))


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
            mock.patch.object(commands, "require_podman", return_value="/usr/bin/podman"),
            mock.patch.object(commands, "image_ready", return_value=True),
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
            mount = command[index + 1].rsplit(":", 2)
            if len(mount) == 3 and mount[1] == "/workspace":
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
