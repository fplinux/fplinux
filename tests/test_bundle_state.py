# SPDX-License-Identifier: GPL-2.0-only
"""Tests for atomic bundle generation publication."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    BundleStateError,
    canonical_json_bytes,
    create_bundle_staging,
    discard_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
    published_file_records,
    resolve_current_bundle,
)


class BundleStateTests(unittest.TestCase):
    """Exercise atomic bundle generation publication."""

    def setUp(self) -> None:
        """Create an isolated bundle output directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / "out"

    def _staging(self, marker: str) -> tuple[Path, str]:
        staging = create_bundle_staging(self.output, "demo", "default")
        (staging / "payload").write_text(marker)
        payload = {
            "target": "demo",
            "profile": "default",
            "workspace_digest": "a" * 64,
            "container_image_recipe": "b" * 64,
            "apk_signing_key": "9" * 64,
            "linux_recipe": "c" * 64,
            "device_identity": "d" * 64,
            "rootfs_receipt": {"recipe": "e" * 64, "sha256": "f" * 64},
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "files": published_file_records(staging),
        }
        generation = hashlib.sha256(canonical_json_bytes(payload)).hexdigest()
        manifest = {**payload, "generation": generation}
        (staging / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        return staging, generation

    def test_publish_and_resolve_current_generation(self) -> None:
        """Publish and resolve the selected generation."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", "default", staging, generation)
        publish_current_bundle(self.output, "demo", "default", published)

        current = resolve_current_bundle(self.output, "demo", "default")

        self.assertEqual(current.path, published)
        self.assertEqual((current.path / "payload").read_text(), "first")

    def test_failed_staging_does_not_replace_last_good_pointer(self) -> None:
        """Discarding failed staging preserves the last good pointer."""
        first, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", "default", first, generation)
        publish_current_bundle(self.output, "demo", "default", published)
        failed = create_bundle_staging(self.output, "demo", "default")
        (failed / "partial").write_text("partial")
        discard_bundle_staging(self.output, "demo", "default", failed)

        current = resolve_current_bundle(self.output, "demo", "default")

        self.assertEqual((current.path / "payload").read_text(), "first")

    def test_new_generation_switch_is_atomic_at_pointer_level(self) -> None:
        """Switching generations leaves the prior pointer valid until publish."""
        first, first_generation = self._staging("first")
        first_path = publish_bundle_generation(
            self.output, "demo", "default", first, first_generation
        )
        publish_current_bundle(self.output, "demo", "default", first_path)
        second, second_generation = self._staging("second")
        second_path = publish_bundle_generation(
            self.output, "demo", "default", second, second_generation
        )

        self.assertEqual(
            (resolve_current_bundle(self.output, "demo", "default").path / "payload").read_text(),
            "first",
        )
        publish_current_bundle(self.output, "demo", "default", second_path)
        self.assertEqual(
            (resolve_current_bundle(self.output, "demo", "default").path / "payload").read_text(),
            "second",
        )

    def test_exact_generation_reuses_existing_directory(self) -> None:
        """Publishing identical content reuses its generation directory."""
        first, generation = self._staging("same")
        first_path = publish_bundle_generation(self.output, "demo", "default", first, generation)
        second, second_generation = self._staging("same")
        second_path = publish_bundle_generation(
            self.output, "demo", "default", second, second_generation
        )
        self.assertEqual(first_path, second_path)

    def test_invalid_pointer_is_a_miss(self) -> None:
        """Reject a malformed current-generation pointer."""
        pointer = self.output / "demo/default.current.json"
        pointer.parent.mkdir(parents=True)
        pointer.write_text(json.dumps({"generation": "bad"}))
        with self.assertRaises(BundleStateError):
            resolve_current_bundle(self.output, "demo", "default")


if __name__ == "__main__":
    unittest.main()
