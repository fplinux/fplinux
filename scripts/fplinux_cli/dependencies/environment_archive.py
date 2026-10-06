# SPDX-License-Identifier: GPL-2.0-only
"""Export and restore the exact Kern build image attached to a dependency snapshot."""

from __future__ import annotations

import json
import shutil
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_file
from fplinux_cli.environment.image_content import validate_image_metadata
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.environment.image_store import (
    current_image_state,
    image_references,
    publish_current_image_state,
    publish_staged_image,
    remove_images,
    tag_image,
    temporary_image_reference,
)
from fplinux_cli.environment.images import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.environment.kern import (
    install_kern,
    kern_box_name,
    kern_environment,
    require_kern,
)

from .snapshots import copy_verified, object_path, prepare_directory

if TYPE_CHECKING:
    from fplinux_cli.reporting.run import RunReporter


_ENVIRONMENT_TIMEOUT = 2 * 60 * 60


def save_environment(directory: Path, reporter: RunReporter) -> dict[str, Any]:
    """Preserve the current image and its measured metadata as snapshot objects."""
    lock = load_container_lock()
    kern = require_kern(lock)
    recipe = container_image_recipe_digest(lock)
    image = container_image_reference(lock, recipe)
    state = current_image_state(kern, image, recipe)
    if state is None:
        fail("current build environment is unavailable; run ./fplinux setup first")
    metadata = _capture_environment_metadata(kern, image, state, reporter)
    metadata_digest = common.sha256_bytes(metadata)
    metadata_path = object_path(directory, metadata_digest)
    prepare_directory(metadata_path.parent)
    replace_file_atomically(metadata_path, metadata, 0o600)
    transport = temporary_image_reference(image, "dependency-transport")
    try:
        with reporter.stage("environment-save") as stage:
            tag_image(kern, image, transport)
            with tempfile.TemporaryDirectory(dir=directory, prefix=".environment-") as temporary:
                archive = Path(temporary) / "environment.tar"
                stage.run(
                    [kern, "save", transport, "-o", str(archive)],
                    cwd=common.ROOT,
                    env=kern_environment(),
                    timeout=_ENVIRONMENT_TIMEOUT,
                )
                _require_image_archive(archive, transport)
                if current_image_state(kern, image, recipe) != state:
                    fail("source build environment changed during snapshot export")
                digest = sha256_file(archive)
                copy_verified(archive, object_path(directory, digest), digest)
                return {
                    "sha256": digest,
                    "bytes": archive.stat().st_size,
                    "reference": image,
                    "transport": transport,
                    "state": state.payload(),
                    "metadata": {"sha256": metadata_digest, "bytes": len(metadata)},
                }
    finally:
        current = image_references(kern)
        remove_images(kern, {transport} & set(current))


def _capture_environment_metadata(
    kern: str, image: str, state: ImageState, reporter: RunReporter
) -> bytes:
    collector = common.ROOT / "scripts/fplinux_cli/environment/image_content.py"
    with reporter.stage("environment-metadata") as stage:
        result = stage.capture(
            [
                kern,
                "box",
                kern_box_name("dependency-metadata"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--cpus",
                "2",
                "--quiet",
                "--volume",
                f"{collector}:/tmp/fplinux-dependency-content.py:ro",
                "--",
                "python3",
                "-B",
                "/tmp/fplinux-dependency-content.py",  # noqa: S108 -- private read-only box mount.
                "--metadata",
            ],
            cwd=common.ROOT,
            env=kern_environment(),
            timeout=300,
        )
        if result.returncode:
            fail("build environment metadata capture failed")
        try:
            records = validate_image_metadata(json.loads(result.stdout))
        except ValueError as error:
            fail(f"build environment metadata is invalid: {error}")
        encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
        if common.sha256_bytes(encoded) != state.image_content:
            fail("captured build environment metadata does not match its measured content")
        return canonical_json_bytes(records)


def restore_environment(
    directory: Path, environment: dict[str, Any], reporter: RunReporter
) -> None:
    """Restore image metadata and publish only a matching installed-content state."""
    lock = load_container_lock()
    recipe = container_image_recipe_digest(lock)
    reference = container_image_reference(lock, recipe)
    state = ImageState(**environment["state"])
    if state.container_image_recipe != recipe or environment["reference"] != reference:
        fail("saved build environment does not match this checkout's exact image recipe")
    transport = environment["transport"]
    archive = object_path(directory, environment["sha256"])
    _require_image_archive(archive, transport)
    kern = install_kern(lock, offline=True)
    staging = temporary_image_reference(reference, "staging")
    try:
        with reporter.stage("environment-load") as stage:
            stage.run(
                [kern, "load", "-i", str(archive)],
                cwd=common.ROOT,
                env=kern_environment(),
                timeout=_ENVIRONMENT_TIMEOUT,
            )
        parent = common.ROOT / ".cache/kern"
        prepare_directory(parent)
        with tempfile.TemporaryDirectory(dir=parent, prefix="dependency-restore-") as temporary:
            context = Path(temporary)
            shutil.copyfile(
                common.ROOT / "scripts/fplinux_cli/environment/image_content.py",
                context / "restore.py",
            )
            shutil.copyfile(
                object_path(directory, environment["metadata"]["sha256"]),
                context / "metadata.json",
            )
            (context / "Containerfile").write_text(
                f"FROM {transport}\n"
                "COPY restore.py metadata.json /tmp/fplinux-dependency-restore/\n"
                "RUN python3 -B /tmp/fplinux-dependency-restore/restore.py --restore-metadata "
                "/tmp/fplinux-dependency-restore/metadata.json "  # noqa: S108 -- private build layer.
                "&& rm -rf /tmp/fplinux-dependency-restore\n",
                encoding="utf-8",
            )
            with reporter.stage("environment-metadata-restore") as stage:
                stage.run(
                    [kern, "build", "-t", staging, "."],
                    cwd=context,
                    env=kern_environment(),
                    timeout=_ENVIRONMENT_TIMEOUT,
                )
        if current_image_state(kern, staging, recipe) != state:
            fail("restored build environment failed its exact installed-content check")
        publish_staged_image(
            kern,
            staging,
            reference,
            lambda candidate: current_image_state(kern, candidate, recipe) == state,
        )
        publish_current_image_state(kern, reference, recipe, state=state)
    finally:
        current = image_references(kern)
        remove_images(kern, {transport, staging} & set(current))


def _require_image_archive(archive: Path, reference: str) -> None:
    try:
        with tarfile.open(archive, "r:") as bundle:
            members = [
                member for member in bundle if member.name.removeprefix("./") == "manifest.json"
            ]
            if len(members) != 1 or not members[0].isfile():
                fail("Kern export has no unique image manifest")
            source = bundle.extractfile(members[0])
            if source is None:
                fail("Kern export image manifest cannot be read")
            with source:
                manifest = json.load(source)
            if not isinstance(manifest, list) or not any(
                isinstance(item, dict) and reference in item.get("RepoTags", [])
                for item in manifest
            ):
                fail("Kern export does not contain the exact requested image reference")
    except (OSError, tarfile.TarError, ValueError) as error:
        fail(f"Kern export cannot be verified: {error}")
