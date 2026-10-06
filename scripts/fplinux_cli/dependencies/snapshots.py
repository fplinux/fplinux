# SPDX-License-Identifier: GPL-2.0-only
"""Validate, preserve and restore exact dependency snapshot objects."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tempfile
from dataclasses import asdict
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_file
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.environment.image_content import validate_image_metadata
from fplinux_cli.environment.image_state import ImageState

from .inputs import DependencyInput

if TYPE_CHECKING:
    from collections.abc import Sequence


def prepare_directory(path: Path) -> None:
    """Prepare an owned directory while rejecting links in its existing parents."""
    for component in (path, *path.parents):
        if component.is_symlink() or (component.exists() and not component.is_dir()):
            fail(f"dependency directory is invalid: {component}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)


def _snapshot_directory(directory: Path, cache: Path) -> Path:
    path = directory.absolute()
    if path.resolve().is_relative_to(cache.resolve()):
        fail("dependency snapshots must be stored outside the working .cache directory")
    prepare_directory(path)
    return path


def object_path(directory: Path, digest: str) -> Path:
    if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
        fail("dependency object SHA-256 is invalid")
    return directory / "objects/sha256" / digest


def copy_verified(source: Path, destination: Path, digest: str, *, mode: int = 0o600) -> None:
    """Stream a large object into its final slot only after validating copied bytes."""
    prepare_directory(destination.parent)
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        fail(f"dependency destination is invalid: {destination}")
    if destination.is_file() and sha256_file(destination) == digest:
        return
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=".input-", delete=False
        ) as output:
            temporary = Path(output.name)
            with source.open("rb") as stream:
                shutil.copyfileobj(stream, output)
            output.flush()
            os.fsync(output.fileno())
        if sha256_file(temporary) != digest:
            fail(f"dependency changed while being copied: {source}")
        temporary.chmod(mode)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _checksum(input_file: DependencyInput, path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    if input_file.size is not None and path.stat().st_size != input_file.size:
        return False
    expected = input_file.checksum or input_file.sha256
    if expected is None or input_file.algorithm not in {"sha256", "sha512"}:
        fail(f"dependency has no supported exact checksum: {input_file.key}")
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, input_file.algorithm).hexdigest()
    return actual == expected


def _source_index(
    sources: Sequence[Path], inputs: Sequence[DependencyInput]
) -> dict[tuple[str, str], Path]:
    """Index explicitly selected originals by their declared native checksums."""
    required = {(item.algorithm, item.checksum or item.sha256) for item in inputs}
    algorithms = sorted({algorithm for algorithm, _ in required})
    matches: dict[tuple[str, str], Path] = {}
    for source in sources:
        candidates = sorted(source.rglob("*")) if source.is_dir() else [source]
        for path in candidates:
            if path.is_symlink() or not path.is_file():
                continue
            for algorithm in algorithms:
                with path.open("rb") as stream:
                    digest = hashlib.file_digest(stream, algorithm).hexdigest()
                if (algorithm, digest) in required:
                    matches.setdefault((algorithm, digest), path)
    return matches


def _saved_input(
    input_file: DependencyInput, cache: Path, sources: dict[tuple[str, str], Path]
) -> Path | None:
    destination = cache / input_file.destination
    if _checksum(input_file, destination):
        return destination
    expected = input_file.checksum or input_file.sha256
    if expected is not None:
        candidate = sources.get((input_file.algorithm, expected))
        if candidate is not None and _checksum(input_file, candidate):
            return candidate
    return None


def _fetch_input(input_file: DependencyInput, cache: Path) -> Path:
    expected = input_file.checksum or input_file.sha256
    if expected is None:
        fail(f"dependency has no exact checksum: {input_file.key}")
    return download_locked_file(
        input_file.url,
        expected,
        cache / input_file.destination,
        algorithm=input_file.algorithm,
        size=input_file.size,
    )


def preserve_inputs(  # noqa: PLR0913 -- closure, storage and acquisition policies are distinct inputs.
    inputs: Sequence[DependencyInput],
    context: dict[str, Any],
    directory: Path,
    *,
    cache: Path,
    offline: bool,
    sources: Sequence[Path] = (),
) -> dict[str, Any]:
    """Preserve explicitly declared files; an absent exact input stops publication."""
    directory = _snapshot_directory(directory, cache)
    for source_path in sources:
        if source_path.is_symlink() or not (source_path.is_dir() or source_path.is_file()):
            fail(f"dependency source path is missing or invalid: {source_path}")
        if source_path.resolve() == cache.resolve():
            fail("select an original input directory instead of the complete working cache")
    source_files = _source_index(sources, inputs)
    records: list[dict[str, Any]] = []
    missing: list[str] = []
    for input_file in sorted(inputs, key=lambda item: item.key):
        source = _saved_input(input_file, cache, source_files)
        if source is None:
            if offline:
                missing.append(f"{input_file.key}: {input_file.url}")
                continue
            try:
                source = _fetch_input(input_file, cache)
            except OSError as error:
                fail(f"dependency download failed: {input_file.key}: {input_file.url}: {error}")
        digest = sha256_file(source)
        copy_verified(source, object_path(directory, digest), digest)
        records.append(
            {"input": asdict(input_file), "sha256": digest, "bytes": source.stat().st_size}
        )
    if missing:
        fail("exact dependency inputs are unavailable:\n" + "\n".join(missing))
    identity = {"context": context, "inputs": records}
    return {
        "snapshot": common.sha256_bytes(canonical_json_bytes(identity)),
        **identity,
        "environment": None,
    }


def publish_snapshot(directory: Path, manifest: dict[str, Any]) -> None:
    """Publish a verified input set without replacing another saved snapshot."""
    destination = directory / "manifest.json"
    if destination.exists() or destination.is_symlink():
        previous = read_snapshot(directory)
        if previous["snapshot"] != manifest["snapshot"]:
            fail(
                "dependency directory already contains another snapshot; choose another directory"
            )
        # The first preserved exact image remains attached to this external input set.
        if previous["environment"] is not None:
            return
    replace_file_atomically(destination, canonical_json_bytes(manifest), 0o600)


def _validate_snapshot_environment(directory: Path, environment: object) -> None:
    """Validate the preserved image, metadata and identities before restoration."""
    if not isinstance(environment, dict) or set(environment) != {
        "sha256",
        "bytes",
        "reference",
        "state",
        "transport",
        "metadata",
    }:
        fail("dependency environment fields are invalid")
    digest = environment["sha256"]
    if not isinstance(digest, str) or type(environment["bytes"]) is not int:
        fail("dependency environment object metadata is invalid")
    object_file = object_path(directory, digest)
    if (
        object_file.is_symlink()
        or not object_file.is_file()
        or object_file.stat().st_size != environment["bytes"]
        or sha256_file(object_file) != digest
    ):
        fail("dependency environment object is missing or mismatched")
    try:
        ImageState(**environment["state"])
    except (TypeError, ValueError) as error:
        fail(f"dependency environment state is invalid: {error}")
    metadata = environment["metadata"]
    if not isinstance(metadata, dict) or set(metadata) != {"sha256", "bytes"}:
        fail("dependency image metadata object fields are invalid")
    if (
        not isinstance(metadata["sha256"], str)
        or type(metadata["bytes"]) is not int
        or metadata["bytes"] < 0
    ):
        fail("dependency image metadata object size or digest is invalid")
    metadata_path = object_path(directory, metadata["sha256"])
    if (
        metadata_path.is_symlink()
        or not metadata_path.is_file()
        or metadata_path.stat().st_size != metadata["bytes"]
        or sha256_file(metadata_path) != metadata["sha256"]
    ):
        fail("dependency image metadata object is missing or mismatched")
    try:
        records = validate_image_metadata(json.loads(metadata_path.read_bytes()))
    except (OSError, ValueError) as error:
        fail(f"dependency image metadata object is invalid: {error}")
    content = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
    if common.sha256_bytes(content) != environment["state"]["image_content"]:
        fail("dependency image metadata does not match its installed-content identity")
    reference, transport = environment["reference"], environment["transport"]
    if (
        not isinstance(reference, str)
        or not isinstance(transport, str)
        or re.fullmatch(
            re.escape(reference) + r"-dependency-transport-[0-9]+-[0-9a-f]{6}", transport
        )
        is None
    ):
        fail("dependency transport image reference is invalid")


def read_snapshot(directory: Path) -> dict[str, Any]:
    """Validate the fixed manifest and every referenced object before restoration."""
    path = directory / "manifest.json"
    if path.is_symlink() or not path.is_file():
        fail(f"dependency manifest is missing or invalid: {path}")
    try:
        value = json.loads(path.read_bytes())
    except (OSError, ValueError) as error:
        fail(f"dependency manifest cannot be read: {path}: {error}")
    if not isinstance(value, dict) or set(value) != {
        "snapshot",
        "context",
        "inputs",
        "environment",
    }:
        fail("dependency manifest fields are invalid")
    records = value["inputs"]
    if not isinstance(value["context"], dict) or not isinstance(records, list):
        fail("dependency manifest context or inputs are invalid")
    keys: set[str] = set()
    destinations: dict[str, str] = {}
    for record in records:
        if not isinstance(record, dict) or set(record) != {"input", "sha256", "bytes"}:
            fail("dependency manifest input record is invalid")
        try:
            input_file = DependencyInput(**record["input"])
        except (TypeError, ValueError) as error:
            fail(f"dependency declaration is invalid: {error}")
        common.relative_name(input_file.destination, field="dependency cache destination")
        if input_file.key in keys:
            fail(f"dependency manifest contains a duplicate input: {input_file.key}")
        keys.add(input_file.key)
        digest = record["sha256"]
        if not isinstance(digest, str) or type(record["bytes"]) is not int or record["bytes"] < 0:
            fail(f"dependency object metadata is invalid: {input_file.key}")
        object_file = object_path(directory, digest)
        previous_digest = destinations.get(input_file.destination)
        if previous_digest is not None and previous_digest != digest:
            fail(f"dependency cache destination has conflicting bytes: {input_file.destination}")
        destinations[input_file.destination] = digest
        if not _checksum(input_file, object_file) or object_file.stat().st_size != record["bytes"]:
            fail(f"dependency object is missing or mismatched: {input_file.key}: {digest}")
        if sha256_file(object_file) != digest:
            fail(f"dependency object SHA-256 mismatch: {input_file.key}: {digest}")
    identity = {"context": value["context"], "inputs": records}
    if value["snapshot"] != common.sha256_bytes(canonical_json_bytes(identity)):
        fail("dependency snapshot identity does not match its manifest")
    environment = value["environment"]
    if environment is not None:
        _validate_snapshot_environment(directory, environment)
    return value


def require_checkout(
    manifest: dict[str, Any], inputs: Sequence[DependencyInput], context: dict[str, Any]
) -> None:
    declarations = [asdict(item) for item in sorted(inputs, key=lambda item: item.key)]
    saved = [record["input"] for record in manifest["inputs"]]
    if saved != declarations or manifest["context"] != context:
        fail("dependency snapshot does not match this checkout's declared external inputs")


def restore_inputs(
    directory: Path,
    inputs: Sequence[DependencyInput],
    context: dict[str, Any],
    *,
    cache: Path,
) -> dict[str, Any]:
    """Restore declared cache inputs after validating the complete snapshot."""
    manifest = read_snapshot(directory)
    require_checkout(manifest, inputs, context)
    restore_manifest_inputs(directory, manifest, cache)
    return manifest


def restore_manifest_inputs(directory: Path, manifest: dict[str, Any], cache: Path) -> None:
    """Copy inputs only after their manifest bytes and checkout selection were checked."""
    for record in manifest["inputs"]:
        destination = cache / record["input"]["destination"]
        if destination.exists() or destination.is_symlink():
            if destination.is_symlink() or not destination.is_file():
                fail(f"dependency cache destination is invalid: {destination}")
            if sha256_file(destination) != record["sha256"]:
                fail(f"dependency cache destination contains different bytes: {destination}")
    for record in manifest["inputs"]:
        destination = cache / record["input"]["destination"]
        copy_verified(object_path(directory, record["sha256"]), destination, record["sha256"])
