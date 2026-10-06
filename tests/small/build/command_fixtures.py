# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic immutable generations shared by command lifecycle cases."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.artifacts.bundles import (
    BUILD_MANIFEST_NAME,
    bundle_pointer,
    publish_current_bundle,
)
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment.image_state import ImageState, publish_image_state
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.bundle_support import file_record


class CommandBundleFixture(unittest.TestCase):
    """Prepare complete test-owned generations and their signing input."""

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
        publish_image_state(self.cache, ImageState("e" * 64, "a" * 64, "a" * 64))
        self.bundle_path = self._create_generation("a" * 64)
        self.bundle = publish_current_bundle(self.output, "phone", self.bundle_path)

    def _manifest(
        self,
        generation: str,
        path: Path,
        profile: str | None = None,
        *,
        runnable: bool = True,
        build_type: str = "release",
    ) -> dict[str, object]:
        """Describe one complete synthetic bundle independently of its resolver."""
        return {
            "workspace_digest": self.snapshot.recipe,
            "container_image_recipe": "e" * 64,
            "container_image_content": "a" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "9" * 64,
            "rootfs_receipt": {"recipe": "f" * 64, "sha256": "0" * 64},
            "boot_artifacts": {"required": [], "runnable": runnable},
            "files": {
                source.relative_to(path).as_posix(): file_record(source)
                for source in path.rglob("*")
                if source.is_file() and source.name != BUILD_MANIFEST_NAME
            },
            "generation": generation,
            "kbuild_receipt": {"recipe": "1" * 64, "sha256": "3" * 64},
            "linux_recipe": "2" * 64,
            "profile": profile,
            "build_type": build_type,
            "target": "phone",
        }

    def _create_generation(
        self,
        generation: str,
        image: bytes = b"ramboot\n",
        *,
        profile: str | None = None,
        runnable: bool = True,
        build_type: str = "release",
    ) -> Path:
        """Write one complete immutable generation without selecting it."""
        slot = self.output / "phone"
        if profile is not None:
            slot = slot / "profiles" / profile
        path = slot / "builds" / build_type / "bundles" / generation
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
            canonical_json_bytes(
                self._manifest(generation, path, profile, runnable=runnable, build_type=build_type)
            )
        )
        return path

    def _clear_current_bundle(self) -> None:
        """Leave complete generations present while making the current receipt miss."""
        bundle_pointer(self.output, "phone").unlink()
