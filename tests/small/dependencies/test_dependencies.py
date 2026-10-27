# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for exact dependency snapshot preservation and recovery."""

from __future__ import annotations

import hashlib
import io
import json
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.dependencies import environment_archive
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.snapshots import (
    preserve_inputs,
    publish_snapshot,
    read_snapshot,
)
from fplinux_cli.dependencies.workflow import create_dependencies, restore_dependencies
from fplinux_cli.environment.definition import environment_recipe_digest
from fplinux_cli.environment.state import EnvironmentState
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
        """Store a native rootfs archive with an independently declared content identity."""
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
        archive_stream = io.BytesIO()
        with tarfile.open(fileobj=archive_stream, mode="w:gz") as archive:
            directory = tarfile.TarInfo("usr")
            directory.type = tarfile.DIRTYPE
            directory.mode = directory_mode
            directory.uid = 1000
            directory.gid = 0
            directory.mtime = int(generation[0], 16)
            archive.addfile(directory)
        archive_bytes = archive_stream.getvalue()
        objects = self.snapshot / "objects/sha256"
        metadata_digest = hashlib.sha256(metadata_bytes).hexdigest()
        archive_digest = hashlib.sha256(archive_bytes).hexdigest()
        (objects / archive_digest).write_bytes(archive_bytes)
        return {
            "sha256": archive_digest,
            "bytes": len(archive_bytes),
            "state": {
                "recipe": recipe,
                "generation": generation,
                "content": metadata_digest,
            },
        }

    def saved_environment(self, *, recipe: str = "a" * 64) -> dict[str, Any]:
        """Attach a controlled native rootfs archive to the input manifest."""
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
        cache = root / "checkout/.data"
        source = cache / "downloads/example.tar"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"abc")
        yield SnapshotCheckout(root, cache, source, root / "saved-inputs")


class DependencySnapshotTests:
    """Restore exact declared bytes without accepting a different dependency set."""

    def test_create_reports_missing_download_before_transfer_and_keeps_cache_hits_quiet(
        self, snapshot_checkout: SnapshotCheckout, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """The CLI identifies an active fetch in stderr and its stage log without cache noise."""
        inputs = [declared_input("second", "downloads/second.tar"), declared_input()]
        expected = "dependencies: download 2/2: second: https://example.invalid/example.tar\n"

        def fetch(*_args: object, **_kwargs: object) -> io.BytesIO:
            assert capsys.readouterr().err.endswith(expected)
            logs = list(
                (snapshot_checkout.cache / "logs/dependencies").glob("*/*-preserve-inputs.log")
            )
            assert len(logs) == 1
            assert expected in logs[0].read_text()
            return io.BytesIO(b"abc")

        with (
            selected_checkout(snapshot_checkout.cache.parent, inputs, {}),
            mock.patch("urllib.request.urlopen", side_effect=fetch),
        ):
            create_dependencies(
                snapshot_checkout.snapshot, offline=False, sources=(), inputs_only=True
            )

        assert (snapshot_checkout.cache / "downloads/second.tar").read_bytes() == b"abc"
        assert len(read_snapshot(snapshot_checkout.snapshot)["inputs"]) == 2
        capsys.readouterr()

        with (
            selected_checkout(snapshot_checkout.cache.parent, inputs, {}),
            mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        ):
            create_dependencies(
                snapshot_checkout.snapshot, offline=False, sources=(), inputs_only=True
            )

        assert "dependencies: download " not in capsys.readouterr().err
        logs = sorted(
            (snapshot_checkout.cache / "logs/dependencies").glob("*/*-preserve-inputs.log")
        )
        assert len(logs) == 2
        assert sum(expected in log.read_text() for log in logs) == 1

    def test_preserve_without_progress_callback_keeps_downloads_quiet(
        self, snapshot_checkout: SnapshotCheckout, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Library consumers can preserve missing inputs without command progress output."""
        with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")):
            manifest = preserve_inputs(
                [declared_input("second", "downloads/second.tar")],
                {},
                snapshot_checkout.snapshot,
                cache=snapshot_checkout.cache,
                offline=False,
            )

        assert len(manifest["inputs"]) == 1
        assert capsys.readouterr() == ("", "")

    def test_round_trip_restores_bytes_and_keeps_unrelated_files(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A fresh cache receives only inputs that its matching declaration selects."""
        context: dict[str, object] = {"providers": ["example=1"]}
        snapshot_checkout.preserve([declared_input()], context)
        new_cache = snapshot_checkout.root / "new-checkout/.data"
        unrelated = new_cache / "other/keep"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_bytes(b"user data")
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            snapshot_checkout.restore([declared_input()], context, new_cache)
        assert ((new_cache / "downloads/example.tar").read_bytes()) == (b"abc")
        assert (unrelated.read_bytes()) == (b"user data")
        assert not ((new_cache / "environment/state.json").exists())

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
        new_cache = snapshot_checkout.root / "restored/.data"
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
        new_cache = snapshot_checkout.root / "new/.data"
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
        new_cache = snapshot_checkout.root / "new/.data"
        with pytest.raises(SystemExit, match="does not match"):
            snapshot_checkout.restore([declared_input()], {"providers": ["example=2"]}, new_cache)
        assert not (new_cache / "downloads").exists()

    def test_object_corruption_is_reported_before_any_restored_file(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Verification detects storage damage before restoration publishes cache inputs."""
        snapshot_checkout.preserve([declared_input()], {})
        (snapshot_checkout.snapshot / "objects/sha256" / _ABC_SHA256).write_bytes(b"bad")
        new_cache = snapshot_checkout.root / "new/.data"
        with pytest.raises(SystemExit, match="object is missing or mismatched"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert not (new_cache / "downloads").exists()

    def test_conflicting_cache_file_is_preserved_before_any_restoration(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Restoration leaves an existing file with different bytes available for inspection."""
        snapshot_checkout.preserve([declared_input()], {})
        new_cache = snapshot_checkout.root / "new/.data"
        existing = new_cache / "downloads/example.tar"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"keep this original")
        with pytest.raises(SystemExit, match="contains different bytes"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert (existing.read_bytes()) == (b"keep this original")

    def test_snapshot_cannot_be_created_inside_managed_data(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Preserved snapshots stay in a user-selected directory outside managed data."""
        with pytest.raises(SystemExit, match="outside the managed data"):
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

    @pytest.mark.parametrize(
        ("recipe", "directory_mode"),
        [
            pytest.param("c" * 64, 493, id="another-environment-recipe"),
            pytest.param("a" * 64, 448, id="another-installed-content"),
        ],
    )
    def test_another_environment_is_rejected_without_replacing_the_snapshot(
        self, snapshot_checkout: SnapshotCheckout, recipe: str, directory_mode: int
    ) -> None:
        """An unchanged external input set cannot silently retain an incompatible rootfs."""
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
        """A new generation for identical declared tools does not replace the saved archive."""
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
        assert candidate["environment"]["sha256"] != saved["environment"]["sha256"]

        publish_snapshot(snapshot_checkout.snapshot, candidate)

        assert manifest_path.read_bytes() == before
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_inputs_only_repeat_keeps_the_saved_environment(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """Saving only originals cannot remove an already preserved build environment."""
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
        """A changed operational generation does not create another native rootfs archive."""
        recipe = environment_recipe_digest()
        saved = snapshot_checkout.saved_environment(recipe=recipe)
        state = EnvironmentState(recipe, "c" * 64, saved["environment"]["state"]["content"])
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
                mock.patch.object(
                    environment_archive, "current_environment_state", return_value=state
                ),
                mock.patch.object(
                    environment_archive,
                    "environment_rootfs",
                    side_effect=AssertionError("saved rootfs must not be exported again"),
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

    @pytest.mark.parametrize("changed_input", ["environment-recipe", "installed-content"])
    def test_saving_a_conflicting_environment_stops_before_export(
        self, snapshot_checkout: SnapshotCheckout, changed_input: str
    ) -> None:
        """An incompatible current rootfs is rejected without adding unused snapshot objects."""
        recipe = environment_recipe_digest()
        saved_recipe = "a" * 64 if changed_input == "environment-recipe" else recipe
        saved = snapshot_checkout.saved_environment(recipe=saved_recipe)
        content = saved["environment"]["state"]["content"]
        if changed_input == "installed-content":
            content = "d" * 64
        state = EnvironmentState(recipe, "c" * 64, content)
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
                mock.patch.object(
                    environment_archive, "current_environment_state", return_value=state
                ),
                mock.patch.object(
                    environment_archive,
                    "environment_rootfs",
                    side_effect=AssertionError("conflicting rootfs must not be exported"),
                ),
                pytest.raises(SystemExit, match="environment"),
            ):
                environment_archive.save_environment(snapshot_checkout.snapshot, reporter)
        finally:
            reporter.finish()

        assert manifest_path.read_bytes() == before_manifest
        assert {path.name: path.read_bytes() for path in objects.iterdir()} == before_objects
        assert read_snapshot(snapshot_checkout.snapshot)["environment"] == saved["environment"]

    def test_missing_environment_archive_prevents_input_restoration(
        self, snapshot_checkout: SnapshotCheckout
    ) -> None:
        """A complete input restore cannot proceed after the saved rootfs object is lost."""
        snapshot_checkout.saved_environment()
        manifest = json.loads((snapshot_checkout.snapshot / "manifest.json").read_bytes())
        digest = manifest["environment"]["sha256"]
        (snapshot_checkout.snapshot / "objects/sha256" / digest).unlink()
        new_cache = snapshot_checkout.root / "restored/.data"
        with pytest.raises(SystemExit, match="environment object is missing"):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert not (new_cache / "downloads").exists()

    @pytest.mark.parametrize("path_case", ["traversal", "link-parent", "hardlink-escape"])
    def test_environment_archive_paths_are_checked_before_input_restoration(
        self, snapshot_checkout: SnapshotCheckout, path_case: str
    ) -> None:
        """Verified object hashes do not authorize writing a path outside the rootfs."""
        snapshot_checkout.saved_environment()
        manifest_path = snapshot_checkout.snapshot / "manifest.json"
        manifest = json.loads(manifest_path.read_bytes())
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            if path_case == "link-parent":
                parent = tarfile.TarInfo("usr")
                parent.type = tarfile.SYMTYPE
                parent.linkname = "/outside"
                archive.addfile(parent)
                member = tarfile.TarInfo("usr/tool")
            else:
                member = tarfile.TarInfo("../outside" if path_case == "traversal" else "tool")
            if path_case == "hardlink-escape":
                member.type = tarfile.LNKTYPE
                member.linkname = "../outside"
                archive.addfile(member)
            else:
                member.size = 3
                archive.addfile(member, io.BytesIO(b"bad"))
        archive_bytes = stream.getvalue()
        digest = hashlib.sha256(archive_bytes).hexdigest()
        (snapshot_checkout.snapshot / "objects/sha256" / digest).write_bytes(archive_bytes)
        manifest["environment"]["sha256"] = digest
        manifest["environment"]["bytes"] = len(archive_bytes)
        manifest_path.write_text(json.dumps(manifest))
        new_cache = snapshot_checkout.root / "restored/.data"
        with pytest.raises(
            SystemExit, match=r"archive (path is invalid|parent is not a directory)"
        ):
            snapshot_checkout.restore([declared_input()], {}, new_cache)
        assert not (new_cache / "downloads").exists()
        assert not (snapshot_checkout.root / "outside").exists()
