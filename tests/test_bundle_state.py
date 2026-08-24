# SPDX-License-Identifier: GPL-2.0-only
"""Tests for immutable bundle generation publication."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    BundleStateError,
    bundle_generations,
    bundle_pointer,
    canonical_json_bytes,
    create_bundle_staging,
    discard_bundle_staging,
    discard_superseded_bundle_generations,
    pointer_bytes,
    publish_bundle_generation,
    publish_current_bundle,
    published_file_records,
    resolve_current_bundle,
)


class BundleStateTests(unittest.TestCase):
    """Exercise immutable bundle generation publication."""

    def setUp(self) -> None:
        """Create an isolated bundle output directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / "out"

    def _staging(
        self,
        marker: str,
        profile: str | None = None,
        *,
        manifest_profile: object = ...,
    ) -> tuple[Path, str]:
        if manifest_profile is ...:
            manifest_profile = profile
        staging = create_bundle_staging(self.output, "demo", profile)
        (staging / "payload").write_text(marker)
        payload = {
            "target": "demo",
            "workspace_digest": "a" * 64,
            "container_image_recipe": "b" * 64,
            "apk_signing_key": "9" * 64,
            "linux_recipe": "c" * 64,
            "device_identity": "d" * 64,
            "rootfs_receipt": {"recipe": "e" * 64, "sha256": "f" * 64},
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "profile": manifest_profile,
            "files": published_file_records(staging),
        }
        generation = hashlib.sha256(canonical_json_bytes(payload)).hexdigest()
        manifest = {**payload, "generation": generation}
        (staging / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        return staging, generation

    def test_publish_and_resolve_current_generation(self) -> None:
        """Publish and resolve the selected generation."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        publish_current_bundle(self.output, "demo", published)

        current = resolve_current_bundle(self.output, "demo")

        self.assertEqual(current.path, published)
        self.assertEqual((current.path / "payload").read_text(), "first")

    def test_discarded_staging_does_not_replace_last_good_pointer(self) -> None:
        """Discarding failed staging preserves the last good pointer."""
        first, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", first, generation)
        publish_current_bundle(self.output, "demo", published)
        failed = create_bundle_staging(self.output, "demo")
        (failed / "partial").write_text("partial")
        discard_bundle_staging(self.output, "demo", failed)

        current = resolve_current_bundle(self.output, "demo")

        self.assertEqual((current.path / "payload").read_text(), "first")

    def test_new_staging_reclaims_crashed_staging_in_only_its_slot(self) -> None:
        """Repeated crash leftovers leave at most one active real staging directory."""
        first = create_bundle_staging(self.output, "demo")
        (first / "partial").write_text("partial")
        other = create_bundle_staging(self.output, "demo", "usb-host-lab")
        generations = bundle_generations(self.output, "demo")
        outside = Path(self.temporary.name) / "outside-staging"
        outside.mkdir()
        unsafe = generations / ".stage-link"
        unsafe.symlink_to(outside, target_is_directory=True)

        second = create_bundle_staging(self.output, "demo")

        active = [
            path
            for path in generations.iterdir()
            if path.name.startswith(".stage-") and not path.is_symlink() and path.is_dir()
        ]
        self.assertEqual(active, [second])
        self.assertFalse(first.exists())
        self.assertTrue(other.is_dir())
        self.assertTrue(unsafe.is_symlink())
        discard_bundle_staging(self.output, "demo", second)
        discard_bundle_staging(self.output, "demo", other, "usb-host-lab")

    def test_current_pointer_changes_only_when_new_generation_is_published(self) -> None:
        """A complete unselected generation does not change the current pointer."""
        first, first_generation = self._staging("first")
        first_path = publish_bundle_generation(self.output, "demo", first, first_generation)
        publish_current_bundle(self.output, "demo", first_path)
        second, second_generation = self._staging("second")
        second_path = publish_bundle_generation(self.output, "demo", second, second_generation)

        self.assertEqual(
            (resolve_current_bundle(self.output, "demo").path / "payload").read_text(),
            "first",
        )
        publish_current_bundle(self.output, "demo", second_path)
        self.assertEqual(
            (resolve_current_bundle(self.output, "demo").path / "payload").read_text(),
            "second",
        )

    def test_exact_generation_reuses_existing_directory(self) -> None:
        """Publishing identical content reuses its generation directory."""
        first, generation = self._staging("same")
        first_path = publish_bundle_generation(self.output, "demo", first, generation)
        second, second_generation = self._staging("same")
        second_path = publish_bundle_generation(self.output, "demo", second, second_generation)
        self.assertEqual(first_path, second_path)

    def test_selected_generation_bounds_only_its_managed_slot(self) -> None:
        """A selected slot removes every stale directory without parsing legacy state."""
        first, first_generation = self._staging("first")
        first_path = publish_bundle_generation(self.output, "demo", first, first_generation)
        first_current = publish_current_bundle(self.output, "demo", first_path)
        second, second_generation = self._staging("second")
        second_path = publish_bundle_generation(self.output, "demo", second, second_generation)
        generations = second_path.parent
        incomplete = generations / ("f" * 64)
        incomplete.mkdir()
        unrelated_directory = generations / "unrelated"
        unrelated_directory.mkdir()
        injected_directory = generations / "another-old-directory"
        injected_directory.mkdir()
        unrelated_file = generations / "notes.txt"
        unrelated_file.write_text("keep")
        unrecognized = generations / ("e" * 64)
        unrecognized.mkdir()
        (unrecognized / BUILD_MANIFEST_NAME).write_text(
            json.dumps({"generation": unrecognized.name, "unexpected": "value"})
        )
        stale_stage = generations / ".stage-stale"
        stale_stage.mkdir()
        symlink_target = generations / "preserve-me"
        symlink_target.mkdir()
        symlink = generations / ("d" * 64)
        symlink.symlink_to(symlink_target, target_is_directory=True)
        other_staging, other_generation = self._staging("other", "usb-host-lab")
        other = publish_bundle_generation(
            self.output,
            "demo",
            other_staging,
            other_generation,
            "usb-host-lab",
        )
        publish_current_bundle(self.output, "demo", other, "usb-host-lab")

        self.assertEqual(first_current.path, first_path)
        self.assertEqual(
            {path.name for path in generations.iterdir() if path.is_dir()},
            {
                first_generation,
                second_generation,
                incomplete.name,
                unrecognized.name,
                unrelated_directory.name,
                injected_directory.name,
                stale_stage.name,
                symlink_target.name,
                symlink.name,
            },
        )
        self.assertEqual(
            resolve_current_bundle(self.output, "demo").generation,
            first_generation,
        )

        current = publish_current_bundle(self.output, "demo", second_path)
        discard_superseded_bundle_generations(self.output, "demo", current)

        self.assertEqual(
            {path.name for path in generations.iterdir() if path.is_dir()},
            {
                second_generation,
            },
        )
        self.assertFalse(unrelated_directory.exists())
        self.assertFalse(injected_directory.exists())
        self.assertFalse(symlink_target.exists())
        self.assertTrue(unrelated_file.is_file())
        self.assertTrue(symlink.is_symlink())
        self.assertEqual(
            resolve_current_bundle(self.output, "demo", "usb-host-lab").path,
            other,
        )
        self.assertEqual(
            resolve_current_bundle(self.output, "demo").generation,
            second_generation,
        )

    def test_invalid_pointer_is_a_miss(self) -> None:
        """Reject a malformed current-generation pointer."""
        pointer = self.output / "demo/current.json"
        pointer.parent.mkdir(parents=True)
        pointer.write_text(json.dumps({"generation": "bad"}))
        with self.assertRaises(BundleStateError):
            resolve_current_bundle(self.output, "demo")

    def test_named_profile_uses_an_isolated_slot_and_manifest_identity(self) -> None:
        """The default and named profile cannot select each other's bundle."""
        default_staging, default_generation = self._staging("default")
        default = publish_bundle_generation(
            self.output,
            "demo",
            default_staging,
            default_generation,
        )
        publish_current_bundle(self.output, "demo", default)

        profile = "usb-host-lab"
        profile_staging, profile_generation = self._staging("host", profile)
        profiled = publish_bundle_generation(
            self.output,
            "demo",
            profile_staging,
            profile_generation,
            profile,
        )
        publish_current_bundle(self.output, "demo", profiled, profile)

        self.assertEqual(default.parent, self.output / "demo/bundles")
        self.assertEqual(
            profiled.parent,
            self.output / "demo/profiles/usb-host-lab/bundles",
        )
        self.assertNotEqual(
            bundle_pointer(self.output, "demo"),
            bundle_pointer(self.output, "demo", profile),
        )
        self.assertEqual(
            (resolve_current_bundle(self.output, "demo").path / "payload").read_text(),
            "default",
        )
        self.assertEqual(
            (resolve_current_bundle(self.output, "demo", profile).path / "payload").read_text(),
            "host",
        )

    def test_profile_mismatched_or_legacy_manifest_is_a_cache_miss(self) -> None:
        """A pointer cannot reuse a manifest from another profile or old schema."""
        profile = "usb-host-lab"
        staging, generation = self._staging(
            "wrong-profile",
            profile,
            manifest_profile=None,
        )
        generations = bundle_generations(self.output, "demo", profile)
        published = generations / generation
        staging.replace(published)
        manifest_bytes = (published / BUILD_MANIFEST_NAME).read_bytes()
        pointer = bundle_pointer(self.output, "demo", profile)
        pointer.write_bytes(pointer_bytes(generation, hashlib.sha256(manifest_bytes).hexdigest()))

        with self.assertRaises(BundleStateError):
            resolve_current_bundle(self.output, "demo", profile)

        manifest = json.loads(manifest_bytes)
        del manifest["profile"]
        legacy = canonical_json_bytes(manifest)
        (published / BUILD_MANIFEST_NAME).write_bytes(legacy)
        pointer.write_bytes(pointer_bytes(generation, hashlib.sha256(legacy).hexdigest()))
        with self.assertRaises(BundleStateError):
            resolve_current_bundle(self.output, "demo", profile)

    def test_atomic_pointer_reuses_a_stale_regular_temporary_file(self) -> None:
        """A crashed pointer write remains one bounded file and self-heals next publish."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        pointer = bundle_pointer(self.output, "demo")
        temporary = pointer.with_name(f".{pointer.name}.tmp")
        temporary.write_text("stale")

        publish_current_bundle(self.output, "demo", published)

        self.assertFalse(temporary.exists())
        self.assertEqual(resolve_current_bundle(self.output, "demo").path, published)

    def test_atomic_pointer_does_not_follow_a_stale_cache_symlink(self) -> None:
        """A stale pointer temporary symlink remains untouched instead of being followed."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        pointer = bundle_pointer(self.output, "demo")
        temporary = pointer.with_name(f".{pointer.name}.tmp")
        outside = Path(self.temporary.name) / "outside"
        outside.write_text("keep")
        temporary.symlink_to(outside)

        with self.assertRaises(BundleStateError):
            publish_current_bundle(self.output, "demo", published)

        self.assertTrue(temporary.is_symlink())
        self.assertEqual(outside.read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
