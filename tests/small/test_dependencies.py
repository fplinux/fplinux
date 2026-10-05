# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for exact dependency snapshot preservation and recovery."""

from __future__ import annotations

import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.dependencies import (
    preserve_inputs,
    publish_snapshot,
    read_snapshot,
    restore_inputs,
)
from fplinux_cli.dependency_inputs import DependencyInput

_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


def declared_input(
    key: str = "example", destination: str = "downloads/example.tar"
) -> DependencyInput:
    """Declare a known SHA-256 example independently of the archive implementation."""
    return DependencyInput(
        key, "https://example.invalid/example.tar", _ABC_SHA256, 3, destination, "build source"
    )


class DependencySnapshotTests(unittest.TestCase):
    """Restore exact declared bytes without accepting a different dependency set."""

    def setUp(self) -> None:
        """Prepare one known original input and an independent preservation directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "checkout/.cache"
        self.source = self.cache / "downloads/example.tar"
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b"abc")
        self.snapshot = self.root / "saved-inputs"

    def preserve(
        self, inputs: list[DependencyInput], context: dict[str, object]
    ) -> dict[str, object]:
        """Publish a complete snapshot from the fixture's declared cache files."""
        manifest = preserve_inputs(inputs, context, self.snapshot, cache=self.cache, offline=True)
        publish_snapshot(self.snapshot, manifest)
        return manifest

    def test_round_trip_restores_bytes_and_keeps_unrelated_files(self) -> None:
        """A fresh cache receives only inputs that its matching declaration selects."""
        context: dict[str, object] = {"providers": ["example=1"]}
        expected = self.preserve([declared_input()], context)
        new_cache = self.root / "new-checkout/.cache"
        unrelated = new_cache / "other/keep"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_bytes(b"user data")
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            restored = restore_inputs(self.snapshot, [declared_input()], context, cache=new_cache)
        self.assertEqual((new_cache / "downloads/example.tar").read_bytes(), b"abc")
        self.assertEqual(unrelated.read_bytes(), b"user data")
        self.assertEqual(restored["snapshot"], expected["snapshot"])
        self.assertFalse((new_cache / "host-image-state.json").exists())

    def test_identical_bytes_use_one_object_for_multiple_declared_consumers(self) -> None:
        """Two consumers retain their destinations while sharing one stored byte object."""
        second = self.cache / "downloads/another.tar"
        second.write_bytes(b"abc")
        self.preserve([declared_input(), declared_input("another", "downloads/another.tar")], {})
        objects = list((self.snapshot / "objects/sha256").iterdir())
        self.assertEqual([path.name for path in objects], [_ABC_SHA256])
        new_cache = self.root / "restored/.cache"
        restore_inputs(
            self.snapshot,
            [declared_input(), declared_input("another", "downloads/another.tar")],
            {},
            cache=new_cache,
        )
        self.assertEqual((new_cache / "downloads/another.tar").read_bytes(), b"abc")

    def test_identity_preserves_sequences_and_ignores_mapping_order(self) -> None:
        """Provider order stays meaningful while equivalent mappings identify one set."""
        first = preserve_inputs(
            [declared_input()],
            {"a": 1, "providers": ["a", "b"]},
            self.snapshot,
            cache=self.cache,
            offline=True,
        )
        reordered = preserve_inputs(
            [declared_input()],
            {"providers": ["a", "b"], "a": 1},
            self.snapshot,
            cache=self.cache,
            offline=True,
        )
        changed = preserve_inputs(
            [declared_input()],
            {"a": 1, "providers": ["b", "a"]},
            self.snapshot,
            cache=self.cache,
            offline=True,
        )
        self.assertEqual(first["snapshot"], reordered["snapshot"])
        self.assertNotEqual(first["snapshot"], changed["snapshot"])

    def test_multiple_consumers_can_restore_the_same_exact_cache_destination(self) -> None:
        """Shared package and loader downloads preserve every consumer's declaration."""
        inputs = [declared_input(), declared_input("another-consumer")]
        self.preserve(inputs, {})
        new_cache = self.root / "new/.cache"
        restore_inputs(self.snapshot, inputs, {}, cache=new_cache)
        self.assertEqual((new_cache / "downloads/example.tar").read_bytes(), b"abc")
        self.assertEqual(len(read_snapshot(self.snapshot)["inputs"]), 2)

    def test_missing_exact_inputs_reports_all_urls_without_publishing_manifest(self) -> None:
        """Offline preservation identifies unavailable versions without substituting files."""
        self.source.unlink()
        inputs = [declared_input(), declared_input("second", "downloads/second.tar")]
        with self.assertRaisesRegex(
            SystemExit, "example: https://example.invalid/example.tar"
        ) as error:
            preserve_inputs(inputs, {}, self.snapshot, cache=self.cache, offline=True)
        self.assertIn("second: https://example.invalid/example.tar", str(error.exception))
        self.assertFalse((self.snapshot / "manifest.json").exists())

    def test_declared_local_source_is_verified_before_preservation(self) -> None:
        """An additional directory supplies the exact original named in the declaration."""
        self.source.unlink()
        originals = self.root / "originals"
        originals.mkdir()
        (originals / "renamed-original.tar").write_bytes(b"abc")
        (originals / "unrelated-key").write_bytes(b"private data")
        manifest = preserve_inputs(
            [declared_input()],
            {},
            self.snapshot,
            cache=self.cache,
            offline=True,
            sources=[originals],
        )
        publish_snapshot(self.snapshot, manifest)
        self.assertEqual(len(read_snapshot(self.snapshot)["inputs"]), 1)
        self.assertEqual((self.snapshot / "objects/sha256" / _ABC_SHA256).read_bytes(), b"abc")
        self.assertFalse((self.snapshot / "unrelated-key").exists())

    def test_mismatched_snapshot_is_rejected_before_writing_cache(self) -> None:
        """A snapshot cannot seed a checkout selecting a different provider."""
        self.preserve([declared_input()], {"providers": ["example=1"]})
        new_cache = self.root / "new/.cache"
        with self.assertRaisesRegex(SystemExit, "does not match"):
            restore_inputs(
                self.snapshot, [declared_input()], {"providers": ["example=2"]}, cache=new_cache
            )
        self.assertFalse(new_cache.exists())

    def test_object_corruption_is_reported_before_any_restored_file(self) -> None:
        """Verification detects storage damage before restoration publishes cache inputs."""
        self.preserve([declared_input()], {})
        (self.snapshot / "objects/sha256" / _ABC_SHA256).write_bytes(b"bad")
        new_cache = self.root / "new/.cache"
        with self.assertRaisesRegex(SystemExit, "object is missing or mismatched"):
            restore_inputs(self.snapshot, [declared_input()], {}, cache=new_cache)
        self.assertFalse(new_cache.exists())

    def test_conflicting_cache_file_is_preserved_before_any_restoration(self) -> None:
        """Restoration leaves an existing file with different bytes available for inspection."""
        self.preserve([declared_input()], {})
        new_cache = self.root / "new/.cache"
        existing = new_cache / "downloads/example.tar"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"keep this original")
        with self.assertRaisesRegex(SystemExit, "contains different bytes"):
            restore_inputs(self.snapshot, [declared_input()], {}, cache=new_cache)
        self.assertEqual(existing.read_bytes(), b"keep this original")

    def test_snapshot_cannot_be_created_in_disposable_working_cache(self) -> None:
        """Preserved inputs stay outside the cache that normal working cleanup replaces."""
        with self.assertRaisesRegex(SystemExit, "outside the working"):
            preserve_inputs(
                [declared_input()], {}, self.cache / "archive", cache=self.cache, offline=True
            )
        self.assertFalse((self.cache / "archive").exists())

    def test_sha512_download_is_verified_and_stored_with_sha256_object_name(self) -> None:
        """A native SHA-512 source declaration preserves its exact original byte stream."""
        declaration = DependencyInput(
            "native",
            "https://example.invalid/native.tar",
            None,
            None,
            "downloads/native.tar",
            "build source",
            checksum=hashlib.sha512(b"abc").hexdigest(),
            algorithm="sha512",
        )
        with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")):
            manifest = preserve_inputs(
                [declaration], {}, self.snapshot, cache=self.cache, offline=False
            )
        publish_snapshot(self.snapshot, manifest)
        self.assertEqual((self.snapshot / "objects/sha256" / _ABC_SHA256).read_bytes(), b"abc")
        self.assertEqual(read_snapshot(self.snapshot)["inputs"][0]["input"]["algorithm"], "sha512")

    def test_manifest_cannot_restore_a_path_outside_cache(self) -> None:
        """An untrusted archive cannot select a destination beyond its cache root."""
        self.preserve([declared_input()], {})
        path = self.snapshot / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["inputs"][0]["input"]["destination"] = "../outside"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(SystemExit, "relative path"):
            read_snapshot(self.snapshot)
        self.assertFalse((self.root / "outside").exists())

    def saved_environment(self) -> dict[str, object]:
        """Attach literal installed metadata and transport bytes to the input manifest."""
        manifest = preserve_inputs(
            [declared_input()], {}, self.snapshot, cache=self.cache, offline=True
        )
        metadata_bytes = b'[{"gid":0,"mode":493,"path":"usr","type":"directory","uid":1000}]'
        archive_bytes = b"native image transport"
        objects = self.snapshot / "objects/sha256"
        metadata_digest = hashlib.sha256(metadata_bytes).hexdigest()
        archive_digest = hashlib.sha256(archive_bytes).hexdigest()
        (objects / metadata_digest).write_bytes(metadata_bytes)
        (objects / archive_digest).write_bytes(archive_bytes)
        reference = "localhost/fplinux-build:" + "a" * 64
        manifest["environment"] = {
            "sha256": archive_digest,
            "bytes": len(archive_bytes),
            "reference": reference,
            "transport": reference + "-dependency-transport-7-abcdef",
            "state": {
                "container_image_recipe": "a" * 64,
                "image_generation": "b" * 64,
                "image_content": (
                    "a60301a02c9095ccbf6f3280eb8922a0bdc62ea6dfbdcfeb08968e4decb53801"
                ),
            },
            "metadata": {"sha256": metadata_digest, "bytes": len(metadata_bytes)},
        }
        publish_snapshot(self.snapshot, manifest)
        return manifest

    def test_environment_metadata_is_verified_with_its_measured_identity(self) -> None:
        """The reader accepts the captured ownership object bound to its content identity."""
        manifest = self.saved_environment()
        self.assertEqual(read_snapshot(self.snapshot)["environment"], manifest["environment"])

    def test_missing_environment_metadata_prevents_input_restoration(self) -> None:
        """A complete input restore cannot proceed after the image metadata object is lost."""
        self.saved_environment()
        manifest = json.loads((self.snapshot / "manifest.json").read_bytes())
        digest = manifest["environment"]["metadata"]["sha256"]
        (self.snapshot / "objects/sha256" / digest).unlink()
        new_cache = self.root / "restored/.cache"
        with self.assertRaisesRegex(SystemExit, "metadata object is missing"):
            restore_inputs(self.snapshot, [declared_input()], {}, cache=new_cache)
        self.assertFalse(new_cache.exists())

    def test_image_metadata_must_match_the_declared_original_content(self) -> None:
        """Verified storage bytes alone do not authorize applying another image's owners."""
        self.saved_environment()
        manifest_path = self.snapshot / "manifest.json"
        manifest = json.loads(manifest_path.read_bytes())
        manifest["environment"]["state"]["image_content"] = "c" * 64
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(SystemExit, "metadata does not match"):
            read_snapshot(self.snapshot)


if __name__ == "__main__":
    unittest.main()
