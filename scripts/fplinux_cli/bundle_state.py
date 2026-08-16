# SPDX-License-Identifier: GPL-2.0-only
"""Atomically publish and resolve immutable target bundle generations."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

BUILD_MANIFEST_NAME = "build-manifest.json"
BUILD_MANIFEST_FIELDS = frozenset(
    {
        "buildroot_receipt",
        "container_image_recipe",
        "device_identity",
        "files",
        "generation",
        "kbuild_receipt",
        "linux_recipe",
        "profile",
        "target",
        "workspace_digest",
    }
)


class BundleStateError(RuntimeError):
    """Report a missing or incomplete current bundle."""


@dataclass(frozen=True)
class CurrentBundle:
    """One bundle generation selected by the current pointer."""

    path: Path
    generation: str
    manifest_sha256: str
    manifest_bytes: bytes


def canonical_json_bytes(value: object) -> bytes:
    """Encode deterministic JSON used for content identities."""
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode()


def bundle_generations(output: Path, target: str, profile: str) -> Path:
    """Return the immutable bundle-generation directory."""
    _component(target, "target")
    _component(profile, "profile")
    return output / target / "bundles" / profile


def bundle_pointer(output: Path, target: str, profile: str) -> Path:
    """Return the current-generation pointer path."""
    _component(target, "target")
    _component(profile, "profile")
    return output / target / f"{profile}.current.json"


def published_file_records(directory: Path) -> dict[str, dict[str, int | str]]:
    """Hash regular bundle payload files for the build manifest."""
    records: dict[str, dict[str, int | str]] = {}
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.name == BUILD_MANIFEST_NAME:
            continue
        relative = path.relative_to(directory).as_posix()
        records[relative] = {
            "mode": path.stat().st_mode & 0o777,
            "sha256": _sha256_file(path),
            "size": path.stat().st_size,
        }
    return records


def create_bundle_staging(output: Path, target: str, profile: str) -> Path:
    """Create one private staging directory beside its future generations."""
    generations = bundle_generations(output, target, profile)
    generations.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=generations, prefix=".stage-"))


def discard_bundle_staging(output: Path, target: str, profile: str, staging: Path) -> None:
    """Discard only the private staging directory created for this bundle."""
    generations = bundle_generations(output, target, profile)
    if staging.parent != generations or not staging.name.startswith(".stage-"):
        message = "bundle staging directory is outside its generation directory"
        raise BundleStateError(message)
    if staging.exists():
        shutil.rmtree(staging)


def publish_bundle_generation(
    output: Path,
    target: str,
    profile: str,
    staging: Path,
    generation: str,
) -> Path:
    """Rename a complete private staging directory to its immutable identity."""
    if not _is_sha256(generation):
        message = "bundle generation is not a SHA-256 digest"
        raise BundleStateError(message)
    generations = bundle_generations(output, target, profile)
    if staging.parent != generations or not staging.name.startswith(".stage-"):
        message = "bundle staging directory is outside its generation directory"
        raise BundleStateError(message)
    manifest = staging / BUILD_MANIFEST_NAME
    if not manifest.is_file():
        message = "bundle staging directory has no build manifest"
        raise BundleStateError(message)
    destination = generations / generation
    if destination.exists():
        existing = destination / BUILD_MANIFEST_NAME
        if existing.is_file() and existing.read_bytes() == manifest.read_bytes():
            shutil.rmtree(staging)
            return destination
        shutil.rmtree(destination)
    staging.replace(destination)
    return destination


def pointer_bytes(generation: str, manifest_sha256: str) -> bytes:
    """Encode one current-pointer payload."""
    if not _is_sha256(generation) or not _is_sha256(manifest_sha256):
        message = "current bundle pointer contains an invalid digest"
        raise BundleStateError(message)
    return canonical_json_bytes({"generation": generation, "manifest_sha256": manifest_sha256})


def publish_current_bundle(
    output: Path,
    target: str,
    profile: str,
    generation_path: Path,
) -> CurrentBundle:
    """Atomically select one already-published complete generation."""
    generations = bundle_generations(output, target, profile)
    if generation_path.parent != generations or not _is_sha256(generation_path.name):
        message = "bundle generation is outside its generation directory"
        raise BundleStateError(message)
    manifest_path = generation_path / BUILD_MANIFEST_NAME
    if not manifest_path.is_file():
        message = "bundle generation has no build manifest"
        raise BundleStateError(message)
    manifest_bytes = manifest_path.read_bytes()
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    pointer = bundle_pointer(output, target, profile)
    pointer.parent.mkdir(parents=True, exist_ok=True)
    temporary = pointer.with_name(f".{pointer.name}.{os.getpid()}.tmp")
    try:
        temporary.write_bytes(pointer_bytes(generation_path.name, manifest_sha256))
        temporary.replace(pointer)
    finally:
        temporary.unlink(missing_ok=True)
    return CurrentBundle(generation_path, generation_path.name, manifest_sha256, manifest_bytes)


def resolve_current_bundle(output: Path, target: str, profile: str) -> CurrentBundle:
    """Resolve one pointer once; any mismatch is a cache miss to the caller."""
    pointer = bundle_pointer(output, target, profile)
    try:
        value = json.loads(pointer.read_text(encoding="utf-8"))
        generation = value["generation"]
        expected_manifest = value["manifest_sha256"]
    except (OSError, UnicodeDecodeError, ValueError, KeyError, TypeError) as error:
        message = "current bundle pointer is missing or invalid"
        raise BundleStateError(message) from error
    if not _is_sha256(generation) or not _is_sha256(expected_manifest):
        message = "current bundle pointer contains an invalid digest"
        raise BundleStateError(message)
    path = bundle_generations(output, target, profile) / generation
    manifest_path = path / BUILD_MANIFEST_NAME
    try:
        manifest_bytes = manifest_path.read_bytes()
    except OSError as error:
        message = "current bundle generation is incomplete"
        raise BundleStateError(message) from error
    actual_manifest = hashlib.sha256(manifest_bytes).hexdigest()
    if actual_manifest != expected_manifest:
        message = "current bundle manifest differs from its pointer"
        raise BundleStateError(message)
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeDecodeError, ValueError) as error:
        message = "current bundle manifest is invalid"
        raise BundleStateError(message) from error
    if (
        not isinstance(manifest, dict)
        or set(manifest) != BUILD_MANIFEST_FIELDS
        or manifest.get("generation") != generation
    ):
        message = "current bundle manifest has the wrong generation"
        raise BundleStateError(message)
    return CurrentBundle(path, generation, actual_manifest, manifest_bytes)


def _component(value: str, field: str) -> None:
    if not value or value in {".", ".."} or "/" in value or "\0" in value:
        raise BundleStateError(f"invalid {field}: {value!r}")


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _sha256_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()
