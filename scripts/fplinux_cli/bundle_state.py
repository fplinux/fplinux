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
        "rootfs_receipt",
        "container_image_recipe",
        "apk_signing_key",
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


def bundle_slot(output: Path, target: str, profile: str | None = None) -> Path:
    """Return the one managed bundle slot for a target and optional profile."""
    _component(target, "target")
    if profile is None:
        return output / target
    _component(profile, "profile")
    return output / target / "profiles" / profile


def bundle_generations(output: Path, target: str, profile: str | None = None) -> Path:
    """Return the immutable bundle-generation directory."""
    return bundle_slot(output, target, profile) / "bundles"


def bundle_pointer(output: Path, target: str, profile: str | None = None) -> Path:
    """Return the current-generation pointer path."""
    return bundle_slot(output, target, profile) / "current.json"


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


def create_bundle_staging(output: Path, target: str, profile: str | None = None) -> Path:
    """Create one private staging directory beside its future generations."""
    generations = _bundle_generations_directory(output, target, profile, create=True)
    _discard_stale_bundle_staging(generations)
    return Path(tempfile.mkdtemp(dir=generations, prefix=".stage-"))


def discard_bundle_staging(
    output: Path,
    target: str,
    staging: Path,
    profile: str | None = None,
) -> None:
    """Discard only the private staging directory created for this bundle."""
    generations = _bundle_generations_directory(output, target, profile, create=False)
    if (
        staging.parent != generations
        or not staging.name.startswith(".stage-")
        or staging.is_symlink()
    ):
        message = "bundle staging directory is outside its generation directory"
        raise BundleStateError(message)
    if staging.exists():
        shutil.rmtree(staging)


def publish_bundle_generation(
    output: Path,
    target: str,
    staging: Path,
    generation: str,
    profile: str | None = None,
) -> Path:
    """Rename a complete private staging directory to its immutable identity."""
    if not _is_sha256(generation):
        message = "bundle generation is not a SHA-256 digest"
        raise BundleStateError(message)
    generations = _bundle_generations_directory(output, target, profile, create=False)
    if (
        staging.parent != generations
        or not staging.name.startswith(".stage-")
        or staging.is_symlink()
    ):
        message = "bundle staging directory is outside its generation directory"
        raise BundleStateError(message)
    manifest = staging / BUILD_MANIFEST_NAME
    if manifest.is_symlink() or not manifest.is_file():
        message = "bundle staging directory has no build manifest"
        raise BundleStateError(message)
    _validate_manifest(manifest.read_bytes(), target, generation, profile)
    destination = generations / generation
    if destination.is_symlink():
        message = "bundle generation path is not a real directory"
        raise BundleStateError(message)
    if destination.exists():
        if not destination.is_dir():
            message = "bundle generation path is not a real directory"
            raise BundleStateError(message)
        existing = destination / BUILD_MANIFEST_NAME
        if (
            not existing.is_symlink()
            and existing.is_file()
            and existing.read_bytes() == manifest.read_bytes()
        ):
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
    generation_path: Path,
    profile: str | None = None,
) -> CurrentBundle:
    """Atomically select one already-published complete generation."""
    generations = _bundle_generations_directory(output, target, profile, create=False)
    if (
        generation_path.parent != generations
        or not _is_sha256(generation_path.name)
        or generation_path.is_symlink()
        or not generation_path.is_dir()
    ):
        message = "bundle generation is outside its generation directory"
        raise BundleStateError(message)
    manifest_path = generation_path / BUILD_MANIFEST_NAME
    if manifest_path.is_symlink() or not manifest_path.is_file():
        message = "bundle generation has no build manifest"
        raise BundleStateError(message)
    manifest_bytes = manifest_path.read_bytes()
    _validate_manifest(manifest_bytes, target, generation_path.name, profile)
    manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
    pointer = bundle_pointer(output, target, profile)
    if pointer.is_symlink() or (pointer.exists() and not pointer.is_file()):
        message = "current bundle pointer is not a regular file"
        raise BundleStateError(message)
    temporary = pointer.with_name(f".{pointer.name}.tmp")
    if temporary.is_symlink() or (temporary.exists() and not temporary.is_file()):
        message = "current bundle temporary pointer path is not a regular file"
        raise BundleStateError(message)
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW,
            0o600,
        )
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(pointer_bytes(generation_path.name, manifest_sha256))
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(pointer)
    except OSError as error:
        message = "cannot atomically publish current bundle pointer"
        raise BundleStateError(message) from error
    return CurrentBundle(generation_path, generation_path.name, manifest_sha256, manifest_bytes)


def discard_superseded_bundle_generations(
    output: Path,
    target: str,
    current: CurrentBundle,
    profile: str | None = None,
) -> None:
    """Bound one managed slot to its selected generation and non-directory cache files."""
    generations = _bundle_generations_directory(output, target, profile, create=False)
    selected = generations / current.generation
    if (
        current.path != selected
        or not _is_sha256(current.generation)
        or selected.is_symlink()
        or not selected.is_dir()
    ):
        message = "selected bundle generation is outside its generation directory"
        raise BundleStateError(message)
    for sibling in generations.iterdir():
        if sibling == selected or sibling.is_symlink() or not sibling.is_dir():
            continue
        shutil.rmtree(sibling)


def resolve_current_bundle(
    output: Path,
    target: str,
    profile: str | None = None,
) -> CurrentBundle:
    """Resolve one pointer once; any mismatch is a cache miss to the caller."""
    _bundle_generations_directory(output, target, profile, create=False)
    pointer = bundle_pointer(output, target, profile)
    if pointer.is_symlink() or not pointer.is_file():
        message = "current bundle pointer is missing or invalid"
        raise BundleStateError(message)
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
    if path.is_symlink() or not path.is_dir():
        message = "current bundle generation is incomplete"
        raise BundleStateError(message)
    if manifest_path.is_symlink() or not manifest_path.is_file():
        message = "current bundle generation is incomplete"
        raise BundleStateError(message)
    try:
        manifest_bytes = manifest_path.read_bytes()
    except OSError as error:
        message = "current bundle generation is incomplete"
        raise BundleStateError(message) from error
    actual_manifest = hashlib.sha256(manifest_bytes).hexdigest()
    if actual_manifest != expected_manifest:
        message = "current bundle manifest differs from its pointer"
        raise BundleStateError(message)
    _validate_manifest(manifest_bytes, target, generation, profile)
    return CurrentBundle(path, generation, actual_manifest, manifest_bytes)


def _component(value: object, field: str) -> None:
    if (
        not isinstance(value, str)
        or not value
        or value in {".", ".."}
        or "/" in value
        or "\0" in value
    ):
        raise BundleStateError(f"invalid {field}: {value!r}")


def _bundle_generations_directory(
    output: Path,
    target: str,
    profile: str | None,
    *,
    create: bool,
) -> Path:
    """Return a real slot-local generations directory without following cache links."""
    slot = bundle_slot(output, target, profile)
    _require_real_directory(output, create=create)
    relative = slot.relative_to(output)
    current = output
    for component in relative.parts:
        current /= component
        _require_real_directory(current, create=create)
    generations = slot / "bundles"
    _require_real_directory(generations, create=create)
    return generations


def _require_real_directory(path: Path, *, create: bool) -> None:
    """Reject symlinked or non-directory state in the managed cache hierarchy."""
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        if not create:
            message = "bundle generation directory is missing"
            raise BundleStateError(message) from None
        path.mkdir()
        metadata = path.lstat()
    except OSError as error:
        message = "cannot inspect bundle generation directory"
        raise BundleStateError(message) from error
    if not metadata or path.is_symlink() or not path.is_dir():
        message = "bundle generation directory is not a real directory"
        raise BundleStateError(message)


def _discard_stale_bundle_staging(generations: Path) -> None:
    """Discard only real stale private staging directories in one managed slot."""
    for sibling in generations.iterdir():
        if not sibling.name.startswith(".stage-") or sibling.is_symlink() or not sibling.is_dir():
            continue
        shutil.rmtree(sibling)


def _validate_manifest(
    manifest_bytes: bytes,
    target: str,
    generation: str,
    profile: str | None,
) -> None:
    """Require a manifest whose exact slot identity matches its containing generation."""
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeDecodeError, ValueError) as error:
        message = "current bundle manifest is invalid"
        raise BundleStateError(message) from error
    if (
        not isinstance(manifest, dict)
        or set(manifest) != BUILD_MANIFEST_FIELDS
        or manifest.get("generation") != generation
        or manifest.get("target") != target
        or manifest.get("profile") != profile
    ):
        message = "current bundle manifest has the wrong slot identity"
        raise BundleStateError(message)


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
