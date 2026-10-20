# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for exact dependency snapshot preservation and recovery."""

from __future__ import annotations

import hashlib
import io
import json
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.cli.dependencies import restore_dependencies
from fplinux_cli.dependencies import environment_archive
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.snapshots import (
    preserve_inputs,
    publish_snapshot,
    read_snapshot,
)
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.environment.images import container_image_recipe_digest
from fplinux_cli.reporting.run import RunReporter

from tests.small.dependencies.snapshot_fixtures import selected_checkout

if TYPE_CHECKING:
    from collections.abc import Iterator
    from typing import Any


_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


def declared_input(
    key: str = "example", destination: str = "downloads/example.tar"
) -> DependencyInput:
    """Declare a known SHA-256 example independently of the archive implementation."""
    return DependencyInput(
        key, "https://example.invalid/example.tar", _ABC_SHA256, 3, destination, "build source"
    )


@dataclass(frozen=True)
class SnapshotCheckout:
    """Own the original cache, archive and restore operations of one scenario."""

    root: Path
    cache: Path
    source: Path
    snapshot: Path

    def preserve(
        self, inputs: list[DependencyInput], context: dict[str, object]
    ) -> dict[str, object]:
        """Publish a complete snapshot from the fixture's declared cache files."""
        manifest = preserve_inputs(inputs, context, self.snapshot, cache=self.cache, offline=True)
        publish_snapshot(self.snapshot, manifest)
        return manifest

    def restore(
        self, inputs: list[DependencyInput], context: dict[str, object], cache: Path
    ) -> None:
        """Run input restoration with controlled declarations in a temporary checkout."""
        with selected_checkout(cache.parent, inputs, context):
            restore_dependencies(self.snapshot, inputs_only=True)

    def environment_objects(
        self,
        *,
        recipe: str = "a" * 64,
        generation: str = "b" * 64,
        directory_mode: int = 493,
    ) -> dict[str, Any]:
        """Store controlled installed metadata and a distinct export for each generation."""
        metadata_bytes = json.dumps(
            [
                {
                    "gid": 0,
                    "mode": directory_mode,
                    "path": "usr",
                    "type": "directory",
                    "uid": 1000,
                }
            ],
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        archive_bytes = b"native image transport " + generation.encode()
        objects = self.snapshot / "objects/sha256"
        metadata_digest = hashlib.sha256(metadata_bytes).hexdigest()
        archive_digest = hashlib.sha256(archive_bytes).hexdigest()
        (objects / metadata_digest).write_bytes(metadata_bytes)
        (objects / archive_digest).write_bytes(archive_bytes)
        reference = "localhost/fplinux-build:" + recipe
        return {
            "sha256": archive_digest,
            "bytes": len(archive_bytes),
            "reference": reference,
            "transport": reference + "-dependency-transport-7-" + generation[:6],
            "state": {
                "container_image_recipe": recipe,
                "image_generation": generation,
                "image_content": metadata_digest,
            },
            "metadata": {"sha256": metadata_digest, "bytes": len(metadata_bytes)},
        }

    def saved_environment(self, *, recipe: str = "a" * 64) -> dict[str, Any]:
        """Attach controlled installed metadata and transport bytes to the input manifest."""
        manifest = preserve_inputs(
            [declared_input()], {}, self.snapshot, cache=self.cache, offline=True
        )
        manifest["environment"] = self.environment_objects(recipe=recipe)
        publish_snapshot(self.snapshot, manifest)
        return manifest


@pytest.fixture
def snapshot_checkout() -> Iterator[SnapshotCheckout]:
    """Remove each scenario's archive and working caches as soon as it finishes."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        cache = root / "checkout/.cache"
        source = cache / "downloads/example.tar"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"abc")
        yield SnapshotCheckout(root, cache, source, root / "saved-inputs")


class DependencySnapshotTests:
    """Restore exact declared bytes without accepting a different dependency set."""

    def test_round_trip_restores_bytes_and_keeps_unrelated_files(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A fresh cache receives only inputs that its matching declaration selects."""
        context: dict[str, object] = {"providers": ["example=1"]}
        snapshot_checkout.preserve([declared_input()], context)
        new_cache = snapshot_checkout.root / "new-checkout/.cache"
        unrelated = new_cache / "other/keep"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_bytes(b"user data")
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            snapshot_checkout.restore([declared_input()], context, new_cache)
        assert ((new_cache / "downloads/example.tar").read_bytes()) == (b"abc")
        assert (unrelated.read_bytes()) == (b"user data")
        assert not ((new_cache / "host-image-state.json").exists())

    def test_identical_bytes_use_one_object_for_multiple_declared_consumers(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Two consumers retain their destinations while sharing one stored byte object."""
        second = snapshot_checkout.cache / "downloads/another.tar"
        second.write_bytes(b"abc")
        snapshot_checkout.preserve(
            [declared_input(), declared_input("another", "downloads/another.tar")], {}
        )
        objects = list((snapshot_checkout.snapshot / "objects/sha256").iterdir())
        assert ([path.name for path in objects]) == ([_ABC_SHA256])
        new_cache = snapshot_checkout.root / "restored/.cache"
        snapshot_checkout.restore(
            [declared_input(), declared_input("another", "downloads/another.tar")],
            {},
            new_cache,
        )
        assert ((new_cache / "downloads/another.tar").read_bytes()) == (b"abc")

    def test_identity_preserves_sequences_and_ignores_mapping_order(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Provider order stays meaningful while equivalent mappings identify one set."""
        first = preserve_inputs(
            [declared_input()],
            {"a": 1, "providers": ["a", "b"]},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        reordered = preserve_inputs(
            [declared_input()],
            {"providers": ["a", "b"], "a": 1},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        changed = preserve_inputs(
            [declared_input()],
            {"a": 1, "providers": ["b", "a"]},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        assert (first["snapshot"]) == (reordered["snapshot"])
        assert (first["snapshot"]) != (changed["snapshot"])

    def test_multiple_consumers_can_restore_the_same_exact_cache_destination(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Shared package and loader downloads preserve every consumer's declaration."""
        inputs = [declared_input(), declared_input("another-consumer")]
        snapshot_checkout.preserve(inputs, {})
        new_cache = snapshot_checkout.root / "new/.cache"
        snapshot_checkout.restore(inputs, {}, new_cache)
        assert ((new_cache / "downloads/example.tar").read_bytes()) == (b"abc")
        assert (len(read_snapshot(snapshot_checkout.snapshot)["inputs"])) == (2)

    def test_missing_exact_inputs_reports_all_urls_without_publishing_manifest(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Offline preservation identifies unavailable versions without substituting files."""
        snapshot_checkout.source.unlink()
        inputs = [declared_input(), declared_input("second", "downloads/second.tar")]
        with pytest.raises(
            SystemExit, match=r"example: https://example.invalid/example.tar"
        ) as error:
            preserve_inputs(
                inputs, {}, snapshot_checkout.snapshot, cache=snapshot_checkout.cache, offline=True
            )
        assert ("second: https://example.invalid/example.tar") in (str(error.value))
        assert not ((snapshot_checkout.snapshot / "manifest.json").exists())

    def test_declared_local_source_is_verified_before_preservation(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """An additional directory supplies the exact original named in the declaration."""
        snapshot_checkout.source.unlink()
        originals = snapshot_checkout.root / "originals"
        originals.mkdir()
        (originals / "renamed-original.tar").write_bytes(b"abc")
        (originals / "unrelated-key").write_bytes(b"private data")
        manifest = preserve_inputs(
            [declared_input()],
            {},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
            sources=[originals],
        )
        publish_snapshot(snapshot_checkout.snapshot, manifest)
        assert (len(read_snapshot(snapshot_checkout.snapshot)["inputs"])) == (1)
        assert ((snapshot_checkout.snapshot / "objects/sha256" / _ABC_SHA256).read_bytes()) == (
            b"abc"
        )
        assert not ((snapshot_checkout.snapshot / "unrelated-key").exists())

    def test_mismatched_snapshot_is_rejected_before_writing_cache(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A snapshot cannot seed a checkout selecting a different provider."""
        snapshot_checkout.preserve([declared_input()], {"providers": ["example=1"]})
        new_cache = snapshot_checkout.root / "new/.cache"
        with pytest.raises(SystemExit, match="does not match"):
            snapshot_checkout.restore([declared_input()], {"providers": ["example=2"]}, new_cache)
        assert not (new_cache / "downloads").exists()

    def test_object_corruption_is_reported_before_any_restored_file(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Verification detects storage damage before restoration publishes cache inputs."""
        snapshot_checkout.preserve([declared_input()], {})
        (snapshot_checkout.snapshot / "objects/sha256" / _ABC_SHA256).write_bytes(b"bad")
        new_cache = snapshot_checkout.root / "new/.cache"
        with pytest.raises(SystemExit, match="object is missing or mismatched"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert not (new_cache / "downloads").exists()

    def test_conflicting_cache_file_is_preserved_before_any_restoration(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Restoration leaves an existing file with different bytes available for inspection."""
        snapshot_checkout.preserve([declared_input()], {})
        new_cache = snapshot_checkout.root / "new/.cache"
        existing = new_cache / "downloads/example.tar"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"keep this original")
        with pytest.raises(SystemExit, match="contains different bytes"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert (existing.read_bytes()) == (b"keep this original")

    def test_snapshot_cannot_be_created_in_disposable_working_cache(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Preserved inputs stay outside the cache that normal working cleanup replaces."""
        with pytest.raises(SystemExit, match="outside the working"):
            preserve_inputs(
                [declared_input()],
                {},
                snapshot_checkout.cache / "archive",
                cache=snapshot_checkout.cache,
                offline=True,
            )
        assert not ((snapshot_checkout.cache / "archive").exists())

    def test_sha512_download_is_verified_and_stored_with_sha256_object_name(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
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
                [declaration],
                {},
                snapshot_checkout.snapshot,
                cache=snapshot_checkout.cache,
                offline=False,
            )
        publish_snapshot(snapshot_checkout.snapshot, manifest)
        assert ((snapshot_checkout.snapshot / "objects/sha256" / _ABC_SHA256).read_bytes()) == (
            b"abc"
        )
        assert (read_snapshot(snapshot_checkout.snapshot)["inputs"][0]["input"]["algorithm"]) == (
            "sha512"
        )

    def test_manifest_cannot_restore_a_path_outside_cache(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """An untrusted archive cannot select a destination beyond its cache root."""
        snapshot_checkout.preserve([declared_input()], {})
        path = snapshot_checkout.snapshot / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["inputs"][0]["input"]["destination"] = "../outside"
        path.write_text(json.dumps(manifest))
        with pytest.raises(SystemExit, match="relative path"):
            read_snapshot(snapshot_checkout.snapshot)
        assert not ((snapshot_checkout.root / "outside").exists())

    def test_environment_metadata_is_verified_with_its_measured_identity(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """The reader accepts the captured ownership object bound to its content identity."""
        manifest = snapshot_checkout.saved_environment()
        assert (
            (read_snapshot(snapshot_checkout.snapshot)["environment"]) == (manifest["environment"])
        )

    @pytest.mark.parametrize(
        ("recipe", "directory_mode"),
        [
            pytest.param("c" * 64, 493, id="another-image-recipe"),
            pytest.param("a" * 64, 448, id="another-installed-content"),
        ],
    )
    def test_another_environment_is_rejected_without_replacing_the_snapshot(
        self, snapshot_checkout: SnapshotCheckout, recipe: str, directory_mode: int
    ) -> None:
        """An unchanged external input set cannot silently retain an incompatible image."""
        saved = snapshot_checkout.saved_environment()
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        before = manifest_path.read_bytes()
        candidate = preserve_inputs(
            [declared_input()],
            {},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        candidate["environment"] = snapshot_checkout.environment_objects(
            recipe=recipe, directory_mode=directory_mode
        )

        with pytest.raises(SystemExit, match="environment"):
            publish_snapshot(snapshot_checkout.snapshot, candidate)

        assert manifest_path.read_bytes() == before
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_compatible_environment_keeps_the_first_export_and_generation(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A new transport for identical declared tools does not replace the saved archive."""
        saved = snapshot_checkout.saved_environment()
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        before = manifest_path.read_bytes()
        candidate = preserve_inputs(
            [declared_input()],
            {},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        candidate["environment"] = snapshot_checkout.environment_objects(generation="c" * 64)
        assert candidate["environment"]["transport"] != saved["environment"]["transport"]
        assert candidate["environment"]["sha256"] != saved["environment"]["sha256"]

        publish_snapshot(snapshot_checkout.snapshot, candidate)

        assert manifest_path.read_bytes() == before
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_inputs_only_repeat_keeps_the_saved_environment(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Saving only originals cannot remove an already preserved build image."""
        saved = snapshot_checkout.saved_environment()
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        before = manifest_path.read_bytes()
        candidate = preserve_inputs(
            [declared_input()],
            {},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        assert candidate["environment"] is None

        publish_snapshot(snapshot_checkout.snapshot, candidate)

        assert manifest_path.read_bytes() == before
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_environment_can_complete_an_inputs_only_snapshot(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """The same input set may acquire its first verified build environment."""
        snapshot_checkout.preserve([declared_input()], {})
        candidate = preserve_inputs(
            [declared_input()],
            {},
            snapshot_checkout.snapshot,
            cache=snapshot_checkout.cache,
            offline=True,
        )
        candidate["environment"] = snapshot_checkout.environment_objects()

        publish_snapshot(snapshot_checkout.snapshot, candidate)

        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == candidate["environment"]

    def test_saving_the_same_environment_reuses_verified_objects_before_export(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A changed operational generation does not create another native image archive."""
        recipe = container_image_recipe_digest()
        saved = snapshot_checkout.saved_environment(recipe=recipe)
        state = ImageState(recipe, "c" * 64, saved["environment"]["state"]["image_content"])
        objects = snapshot_checkout.snapshot / "objects/sha256"
        before_objects = {path.name: path.read_bytes() for path in objects.iterdir()}
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        before_manifest = manifest_path.read_bytes()
        reporter = RunReporter(
            "dependencies", snapshot_checkout.root / "logs", "logs", verbose=False
        )
        try:
            with (
                mock.patch.object(environment_archive, "require_kern", return_value="kern"),
                mock.patch.object(environment_archive, "current_image_state", return_value=state),
                mock.patch.object(
                    environment_archive,
                    "_capture_environment_metadata",
                    side_effect=AssertionError("saved image metadata must be reused"),
                ),
                mock.patch.object(
                    environment_archive,
                    "tag_image",
                    side_effect=AssertionError("saved image must not be exported again"),
                ),
            ):
                environment = environment_archive.save_environment(
                    snapshot_checkout.snapshot, reporter
                )
        finally:
            reporter.finish()

        assert environment == saved["environment"]
        assert manifest_path.read_bytes() == before_manifest
        assert {path.name: path.read_bytes() for path in objects.iterdir()} == before_objects

    @pytest.mark.parametrize("changed_input", ["image-recipe", "installed-content"])
    def test_saving_a_conflicting_environment_stops_before_export(
        self, snapshot_checkout: SnapshotCheckout, changed_input: str
    ) -> None:
        """An incompatible current image is rejected without adding unused snapshot objects."""
        recipe = container_image_recipe_digest()
        saved_recipe = "a" * 64 if changed_input == "image-recipe" else recipe
        saved = snapshot_checkout.saved_environment(recipe=saved_recipe)
        content = saved["environment"]["state"]["image_content"]
        if changed_input == "installed-content":
            content = "d" * 64
        state = ImageState(recipe, "c" * 64, content)
        objects = snapshot_checkout.snapshot / "objects/sha256"
        before_objects = {path.name: path.read_bytes() for path in objects.iterdir()}
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        before_manifest = manifest_path.read_bytes()
        reporter = RunReporter(
            "dependencies", snapshot_checkout.root / "logs", "logs", verbose=False
        )
        try:
            with (
                mock.patch.object(environment_archive, "require_kern", return_value="kern"),
                mock.patch.object(environment_archive, "current_image_state", return_value=state),
                mock.patch.object(
                    environment_archive,
                    "_capture_environment_metadata",
                    side_effect=AssertionError("conflicting image metadata must not be captured"),
                ),
                mock.patch.object(
                    environment_archive,
                    "tag_image",
                    side_effect=AssertionError("conflicting image must not be exported"),
                ),
                pytest.raises(SystemExit, match="environment"),
            ):
                environment_archive.save_environment(snapshot_checkout.snapshot, reporter)
        finally:
            reporter.finish()

        assert manifest_path.read_bytes() == before_manifest
        assert {path.name: path.read_bytes() for path in objects.iterdir()} == before_objects
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_missing_environment_metadata_prevents_input_restoration(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A complete input restore cannot proceed after the image metadata object is lost."""
        snapshot_checkout.saved_environment()
        manifest = json.loads((snapshot_checkout.snapshot / "manifest.json").read_bytes())
        digest = manifest["environment"]["metadata"]["sha256"]
        (snapshot_checkout.snapshot / "objects/sha256" / digest).unlink()
        new_cache = snapshot_checkout.root / "restored/.cache"
        with pytest.raises(SystemExit, match="metadata object is missing"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert not (new_cache / "downloads").exists()

    def test_image_metadata_must_match_the_declared_original_content(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Verified storage bytes alone do not authorize applying another image's owners."""
        snapshot_checkout.saved_environment()
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        manifest = json.loads(manifest_path.read_bytes())
        manifest["environment"]["state"]["image_content"] = "c" * 64
        manifest_path.write_text(json.dumps(manifest))
        with pytest.raises(SystemExit, match="metadata does not match"):
            read_snapshot(snapshot_checkout.snapshot)
