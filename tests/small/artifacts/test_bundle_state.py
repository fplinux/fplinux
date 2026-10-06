# SPDX-License-Identifier: GPL-2.0-only
"""Tests for immutable bundle generation publication."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.bundles import (
    BUILD_MANIFEST_NAME,
    BundleStateError,
    bundle_generations,
    bundle_pointer,
    create_bundle_staging,
    discard_bundle_staging,
    discard_superseded_bundle_generations,
    pointer_bytes,
    publish_bundle_generation,
    publish_current_bundle,
    resolve_current_bundle,
)
from fplinux_cli.common import canonical_json_bytes

from tests.bundle_support import file_record

if TYPE_CHECKING:
    from pathlib import Path


class BundleStateTests:
    """Exercise immutable bundle generation publication."""

    @pytest.fixture(autouse=True)
    def _bundle_output(self, tmp_path: Path) -> None:
        """Create an isolated bundle output directory."""
        self.root = tmp_path
        self.output = tmp_path / "out"

    def _staging(
        self,
        marker: str,
        profile: str | None = None,
        *,
        manifest_profile: object = ...,
        build_type: str = "release",
        manifest_build_type: object = ...,
    ) -> tuple[Path, str]:
        if manifest_profile is ...:
            manifest_profile = profile
        if manifest_build_type is ...:
            manifest_build_type = build_type
        staging = create_bundle_staging(self.output, "demo", profile, build_type=build_type)
        (staging / "payload").write_text(marker)
        (staging / "payload").chmod(0o644)
        payload = {
            "target": "demo",
            "workspace_digest": "a" * 64,
            "container_image_recipe": "b" * 64,
            "container_image_content": "2" * 64,
            "apk_signing_key": "9" * 64,
            "linux_recipe": "c" * 64,
            "device_identity": "d" * 64,
            "rootfs_receipt": {"recipe": "e" * 64, "sha256": "f" * 64},
            "boot_artifacts": {"required": []},
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "profile": manifest_profile,
            "build_type": manifest_build_type,
            "files": {"payload": file_record(staging / "payload")},
        }
        generation = hashlib.sha256(canonical_json_bytes(payload)).hexdigest()
        manifest = {**payload, "generation": generation}
        (staging / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        return staging, generation

    def test_build_types_retain_independent_current_generations(self) -> None:
        """Switching type and pruning one slot cannot replace the other type's payload."""
        selected: dict[str, Path] = {}
        for build_type in ("release", "debug"):
            staging, generation = self._staging(build_type, build_type=build_type)
            selected[build_type] = publish_bundle_generation(
                self.output, "demo", staging, generation, build_type=build_type
            )
            current = publish_current_bundle(
                self.output, "demo", selected[build_type], build_type=build_type
            )
            discard_superseded_bundle_generations(
                self.output, "demo", current, build_type=build_type
            )

        for build_type in ("release", "debug", "release"):
            current = resolve_current_bundle(self.output, "demo", build_type=build_type)
            assert (current.path) == (selected[build_type])
            assert ((current.path / "payload").read_text()) == (build_type)

    @pytest.mark.parametrize("manifest_type", ["debug", None], ids=["wrong-type", "missing-type"])
    def test_wrong_or_missing_type_cannot_be_published_into_a_selected_slot(
        self, manifest_type: str | None
    ) -> None:
        """A manifest from another build type cannot authorize a release payload."""
        staging, generation = self._staging("payload", manifest_build_type=manifest_type)
        if manifest_type is None:
            manifest = json.loads((staging / BUILD_MANIFEST_NAME).read_text())
            del manifest["build_type"]
            (staging / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        with pytest.raises(BundleStateError, match="wrong slot identity"):
            publish_bundle_generation(self.output, "demo", staging, generation)

    def test_publish_and_resolve_current_generation(self) -> None:
        """Publish and resolve the selected generation."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        publish_current_bundle(self.output, "demo", published)

        current = resolve_current_bundle(self.output, "demo")

        assert (current.path) == (published)
        assert ((current.path / "payload").read_text()) == ("first")

    def test_discarded_staging_does_not_replace_last_good_pointer(self) -> None:
        """Discarding failed staging preserves the last good pointer."""
        first, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", first, generation)
        publish_current_bundle(self.output, "demo", published)
        failed = create_bundle_staging(self.output, "demo")
        (failed / "partial").write_text("partial")
        discard_bundle_staging(self.output, "demo", failed)

        current = resolve_current_bundle(self.output, "demo")

        assert ((current.path / "payload").read_text()) == ("first")

    def test_new_staging_reclaims_crashed_staging_in_only_its_slot(self) -> None:
        """Repeated crash leftovers leave at most one active real staging directory."""
        first = create_bundle_staging(self.output, "demo")
        (first / "partial").write_text("partial")
        other = create_bundle_staging(self.output, "demo", "usb-host-lab")
        generations = bundle_generations(self.output, "demo")
        outside = self.root / "outside-staging"
        outside.mkdir()
        unsafe = generations / ".stage-link"
        unsafe.symlink_to(outside, target_is_directory=True)

        second = create_bundle_staging(self.output, "demo")

        active = [
            path
            for path in generations.iterdir()
            if path.name.startswith(".stage-") and not path.is_symlink() and path.is_dir()
        ]
        assert (active) == ([second])
        assert not (first.exists())
        assert other.is_dir()
        assert unsafe.is_symlink()
        discard_bundle_staging(self.output, "demo", second)
        discard_bundle_staging(self.output, "demo", other, "usb-host-lab")

    def test_current_pointer_changes_only_when_new_generation_is_published(self) -> None:
        """A complete unselected generation does not change the current pointer."""
        first, first_generation = self._staging("first")
        first_path = publish_bundle_generation(self.output, "demo", first, first_generation)
        publish_current_bundle(self.output, "demo", first_path)
        second, second_generation = self._staging("second")
        second_path = publish_bundle_generation(self.output, "demo", second, second_generation)

        assert ((resolve_current_bundle(self.output, "demo").path / "payload").read_text()) == (
            "first"
        )
        publish_current_bundle(self.output, "demo", second_path)
        assert ((resolve_current_bundle(self.output, "demo").path / "payload").read_text()) == (
            "second"
        )

    def test_exact_generation_reuses_existing_directory(self) -> None:
        """Publishing identical content reuses its generation directory."""
        first, generation = self._staging("same")
        first_path = publish_bundle_generation(self.output, "demo", first, generation)
        original_inode = first_path.stat().st_ino
        second, second_generation = self._staging("same")
        second_path = publish_bundle_generation(self.output, "demo", second, second_generation)
        assert (first_path) == (second_path)
        assert (second_path.stat().st_ino) == (original_inode)
        assert not (second.exists())

    @pytest.mark.parametrize("mutation", ["bytes", "missing", "mode"])
    def test_identical_manifest_repairs_invalid_existing_payload(self, mutation: str) -> None:
        """A valid staging payload replaces damaged bytes, missing files or changed modes."""
        marker = f"same-{mutation}"
        first, generation = self._staging(marker)
        published = publish_bundle_generation(self.output, "demo", first, generation)
        publish_current_bundle(self.output, "demo", published)
        payload = published / "payload"
        if mutation == "bytes":
            payload.write_bytes(b"changed")
        elif mutation == "missing":
            payload.unlink()
        else:
            payload.chmod(0o600)
        second, same_generation = self._staging(marker)

        repaired = publish_bundle_generation(self.output, "demo", second, same_generation)

        assert (same_generation) == (generation)
        assert (repaired) == (published)
        assert (payload.read_bytes()) == (marker.encode())
        assert (payload.stat().st_mode & 0o777) == (0o644)
        assert not (second.exists())
        assert (resolve_current_bundle(self.output, "demo").path) == (published)

    def test_invalid_staging_preserves_the_existing_generation_and_pointer(self) -> None:
        """Failed staging verification cannot destroy the generation awaiting repair."""
        first, generation = self._staging("same")
        published = publish_bundle_generation(self.output, "demo", first, generation)
        publish_current_bundle(self.output, "demo", published)
        pointer = bundle_pointer(self.output, "demo")
        pointer_before = pointer.read_bytes()
        (published / "payload").write_bytes(b"old damage")
        second, same_generation = self._staging("same")
        (second / "payload").write_bytes(b"staging damage")

        with pytest.raises(BundleStateError):
            publish_bundle_generation(self.output, "demo", second, same_generation)

        assert ((published / "payload").read_bytes()) == (b"old damage")
        assert (pointer.read_bytes()) == (pointer_before)
        assert second.is_dir()

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

        assert (first_current.path) == (first_path)
        assert ({path.name for path in generations.iterdir() if path.is_dir()}) == (
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
            }
        )
        assert (resolve_current_bundle(self.output, "demo").generation) == (first_generation)

        current = publish_current_bundle(self.output, "demo", second_path)
        discard_superseded_bundle_generations(self.output, "demo", current)

        assert ({path.name for path in generations.iterdir() if path.is_dir()}) == (
            {
                second_generation,
            }
        )
        assert not (unrelated_directory.exists())
        assert not (injected_directory.exists())
        assert not (symlink_target.exists())
        assert unrelated_file.is_file()
        assert symlink.is_symlink()
        assert (resolve_current_bundle(self.output, "demo", "usb-host-lab").path) == (other)
        assert (resolve_current_bundle(self.output, "demo").generation) == (second_generation)

    def test_malformed_pointer_is_a_miss(self) -> None:
        """Reject a malformed current-generation pointer."""
        bundle_generations(self.output, "demo").mkdir(parents=True)
        pointer = bundle_pointer(self.output, "demo")
        pointer.write_text("{", encoding="utf-8")
        with pytest.raises(
            BundleStateError, match="current bundle pointer is missing or invalid"
        ) as raised:
            resolve_current_bundle(self.output, "demo")
        assert isinstance(raised.value.__cause__, json.JSONDecodeError)

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

        assert (default.parent) == (self.output / "demo/builds/release/bundles")
        assert (profiled.parent) == (
            self.output / "demo/profiles/usb-host-lab/builds/release/bundles"
        )
        assert (bundle_pointer(self.output, "demo")) != (
            bundle_pointer(self.output, "demo", profile)
        )
        assert ((resolve_current_bundle(self.output, "demo").path / "payload").read_text()) == (
            "default"
        )
        assert (
            (resolve_current_bundle(self.output, "demo", profile).path / "payload").read_text()
        ) == ("host")

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

        with pytest.raises(BundleStateError):
            resolve_current_bundle(self.output, "demo", profile)

        manifest = json.loads(manifest_bytes)
        del manifest["profile"]
        legacy = canonical_json_bytes(manifest)
        (published / BUILD_MANIFEST_NAME).write_bytes(legacy)
        pointer.write_bytes(pointer_bytes(generation, hashlib.sha256(legacy).hexdigest()))
        with pytest.raises(BundleStateError):
            resolve_current_bundle(self.output, "demo", profile)

    def test_atomic_pointer_reuses_a_stale_regular_temporary_file(self) -> None:
        """A crashed pointer write remains one bounded file and self-heals next publish."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        pointer = bundle_pointer(self.output, "demo")
        temporary = pointer.with_name(f".{pointer.name}.tmp")
        temporary.write_text("stale")

        publish_current_bundle(self.output, "demo", published)

        assert not (temporary.exists())
        assert (resolve_current_bundle(self.output, "demo").path) == (published)

    def test_atomic_pointer_does_not_follow_a_stale_cache_symlink(self) -> None:
        """A stale pointer temporary symlink remains untouched instead of being followed."""
        staging, generation = self._staging("first")
        published = publish_bundle_generation(self.output, "demo", staging, generation)
        pointer = bundle_pointer(self.output, "demo")
        temporary = pointer.with_name(f".{pointer.name}.tmp")
        outside = self.root / "outside"
        outside.write_text("keep")
        temporary.symlink_to(outside)

        with pytest.raises(BundleStateError):
            publish_current_bundle(self.output, "demo", published)

        assert temporary.is_symlink()
        assert (outside.read_text()) == ("keep")
