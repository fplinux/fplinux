# SPDX-License-Identifier: GPL-2.0-only
"""Exercise image export preservation through a controlled runtime process."""

from __future__ import annotations

import gzip
import hashlib
import json
import os
import shutil
import tarfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.dependencies import environment_archive
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.environment.images import container_image_recipe_digest
from fplinux_cli.reporting.run import RunReporter

if TYPE_CHECKING:
    import pytest

_EXPORT_PEER = (
    Path(__file__).resolve().parents[2]
    / "fixtures/dependencies/environment-export/kern_export_peer.py"
)


def test_saved_environment_compresses_layers_and_preserves_native_image(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The stored plain archive carries lossless gzip layers and unchanged image metadata."""
    peer = tmp_path / "kern"
    shutil.copyfile(_EXPORT_PEER, peer)
    peer.chmod(0o755)
    original = tmp_path / "original-export.tar"
    monkeypatch.setenv("FPLINUX_TEST_ORIGINAL_EXPORT", str(original))
    directory = tmp_path / "snapshot"
    directory.mkdir()
    metadata = [{"gid": 0, "mode": 493, "path": "usr", "type": "directory", "uid": 1000}]
    content = hashlib.sha256(
        json.dumps(metadata, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    recipe = container_image_recipe_digest()
    state = ImageState(recipe, "b" * 64, content)
    image = "localhost/fplinux-build:" + recipe
    transport = image + "-dependency-transport-7-abcdef"
    reporter = RunReporter("dependencies", tmp_path / "logs", "logs", verbose=False)
    try:
        with (
            mock.patch.object(environment_archive, "require_kern", return_value=str(peer)),
            mock.patch.object(environment_archive, "current_image_state", return_value=state),
            mock.patch.object(
                environment_archive, "kern_environment", side_effect=os.environ.copy
            ),
            mock.patch.object(
                environment_archive, "temporary_image_reference", return_value=transport
            ),
            mock.patch.object(environment_archive, "tag_image") as tag,
            mock.patch.object(
                environment_archive, "image_references", return_value=frozenset({transport})
            ),
            mock.patch.object(environment_archive, "remove_images") as remove,
        ):
            environment = environment_archive.save_environment(directory, reporter)
    finally:
        reporter.finish()

    tag.assert_called_once_with(str(peer), image, transport)
    remove.assert_called_once_with(str(peer), {transport})
    archive_path = directory / "objects/sha256" / environment["sha256"]
    assert archive_path.stat().st_size == environment["bytes"]
    with archive_path.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == environment["sha256"]
    metadata_path = directory / "objects/sha256" / environment["metadata"]["sha256"]
    assert json.loads(metadata_path.read_bytes()) == metadata
    assert environment["state"] == state.payload()

    with tarfile.open(original, "r:") as before, tarfile.open(archive_path, "r:") as after:
        assert sorted(before.getnames()) == sorted(after.getnames())
        manifest_source = before.extractfile("./manifest.json")
        manifest_saved = after.extractfile("./manifest.json")
        assert manifest_source is not None
        assert manifest_saved is not None
        manifest_bytes = manifest_source.read()
        assert manifest_saved.read() == manifest_bytes
        manifest = json.loads(manifest_bytes)[0]
        config_name = "./" + manifest["Config"]
        config_source = before.extractfile(config_name)
        config_saved = after.extractfile(config_name)
        assert config_source is not None
        assert config_saved is not None
        config_bytes = config_source.read()
        assert config_saved.read() == config_bytes
        layer_name = "./" + manifest["Layers"][0]
        layer_source = before.extractfile(layer_name)
        layer_saved = after.extractfile(layer_name)
        assert layer_source is not None
        assert layer_saved is not None
        layer_bytes = layer_source.read()
        compressed = layer_saved.read()
        assert compressed[:3] == b"\x1f\x8b\x08"
        assert compressed[3] & 8 == 0
        assert compressed[4:8] == b"\0\0\0\0"
        assert len(compressed) < len(layer_bytes)
        assert gzip.decompress(compressed) == layer_bytes
        config = json.loads(config_bytes)
        assert config["rootfs"]["diff_ids"] == [
            "sha256:" + hashlib.sha256(layer_bytes).hexdigest()
        ]
        for member in before:
            saved_member = after.getmember(member.name)
            actual = dict(saved_member.get_info())
            expected = dict(member.get_info())
            # Re-encoding tar headers recalculates their checksums.
            del actual["chksum"]
            del expected["chksum"]
            if member.name == layer_name:
                expected["size"] = len(compressed)
            assert actual == expected
