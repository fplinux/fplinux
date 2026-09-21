# SPDX-License-Identifier: GPL-2.0-only
"""Identify the pinned build environment and its causal inputs."""

from __future__ import annotations

import hashlib
import re
from typing import TYPE_CHECKING, Any, Protocol

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.values import sha256_value
from fplinux_cli.quality.source_policy import validate_source_policy

if TYPE_CHECKING:
    from pathlib import Path


def load_container_lock() -> dict[str, Any]:
    """Load the pinned project-local Kern and OCI build-environment lock."""
    validate_source_policy()
    path = common.ROOT / "container.lock.toml"
    lock = common.load_toml(path)
    if set(lock) != {"kern", "oci"}:
        fail(f"container lock must contain exactly kern and oci: {path}")

    kern = lock.get("kern")
    if not isinstance(kern, dict) or set(kern) != {
        "version",
        "archive_url",
        "archive_sha256",
        "binary_sha256",
    }:
        fail(f"container lock must define one pinned Kern release: {path}")
    version = kern.get("version")
    archive_url = kern.get("archive_url")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        fail(f"Kern version is invalid: {path}")
    if not isinstance(archive_url, str) or not archive_url.startswith("https://"):
        fail(f"Kern release URL must use HTTPS: {path}")
    for field in ("archive_sha256", "binary_sha256"):
        sha256_value(kern.get(field), f"Kern {field} in {path}")

    oci = lock.get("oci")
    if not isinstance(oci, dict) or set(oci) != {
        "repository",
        "platform",
        "base_repository",
        "base_release",
        "base_rootfs_url",
        "base_rootfs_sha256",
    }:
        fail(f"container lock must define exactly one OCI repository and base rootfs: {path}")
    if oci.get("repository") != "localhost/fplinux-build":
        fail(f"container repository must be localhost/fplinux-build: {path}")
    if oci.get("base_repository") != "localhost/fplinux-alpine-base":
        fail(f"container base repository must be localhost/fplinux-alpine-base: {path}")
    if oci.get("platform") != "linux/amd64":
        fail(f"unsupported container platform: {path}")
    release = oci.get("base_release")
    rootfs_url = oci.get("base_rootfs_url")
    rootfs_sha256 = oci.get("base_rootfs_sha256")
    if not isinstance(release, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", release) is None:
        fail(f"container base release is invalid: {path}")
    if not isinstance(rootfs_url, str) or not rootfs_url.startswith("https://"):
        fail(f"container base rootfs URL must use HTTPS: {path}")
    sha256_value(rootfs_sha256, f"container base rootfs SHA-256 in {path}")
    return lock


class _Hash(Protocol):
    """Describe the incremental hash operation used by recipe encoders."""

    def update(self, data: bytes) -> None:
        """Add bytes to the digest state."""


def _length_prefixed(value: _Hash, data: bytes) -> None:
    """Add one unambiguous byte field to an incremental recipe digest."""
    value.update(len(data).to_bytes(8, "big"))
    value.update(data)


def _stable_regular_file(path: Path) -> tuple[bytes, int]:
    """Read one regular causal recipe input."""
    if path.is_symlink() or not path.is_file():
        fail(f"recipe input is missing or invalid: {path}")
    try:
        return path.read_bytes(), path.stat().st_mode & 0o777
    except OSError as error:
        fail(f"recipe input cannot be read: {path}: {error}")


def file_recipe_digest(paths: list[Path], *, prefix: bytes = b"") -> str:
    """Hash exact logical paths, bytes and modes for one causal file closure."""
    value = hashlib.sha256()
    _length_prefixed(value, prefix)
    for path in paths:
        contents, mode = _stable_regular_file(path)
        relative = path.relative_to(common.ROOT).as_posix()
        _length_prefixed(value, relative.encode())
        _length_prefixed(value, contents)
        _length_prefixed(value, mode.to_bytes(2, "big"))
    return value.hexdigest()


def container_base_image_reference(lock: dict[str, Any] | None = None) -> str:
    """Return the local Kern base tag bound to the exact locked minirootfs bytes."""
    if lock is None:
        lock = load_container_lock()
    oci = lock["oci"]
    return f"{oci['base_repository']}:{oci['base_release']}-{oci['base_rootfs_sha256']}"


def container_image_build_arguments(lock: dict[str, Any] | None = None) -> tuple[str, ...]:
    """Return exact causal Kern build arguments, excluding tag and image-state markers."""
    if lock is None:
        lock = load_container_lock()
    return (
        "-f",
        "Containerfile",
        "--build-arg",
        f"BASE_IMAGE={container_base_image_reference(lock)}",
    )


def container_image_recipe_digest(lock: dict[str, Any] | None = None) -> str:
    """Hash the exact image context, build arguments and pinned Kern binary."""
    if lock is None:
        lock = load_container_lock()
    arguments = (*container_image_build_arguments(lock), ".")
    encoded_arguments = b"".join(
        len(argument.encode()).to_bytes(8, "big") + argument.encode() for argument in arguments
    )
    return file_recipe_digest(
        [
            common.ROOT / ".kernignore",
            common.ROOT / "Containerfile",
            common.ROOT / "package.json",
            common.ROOT / "package-lock.json",
        ],
        prefix=(
            b"fplinux.container-image-recipe\0"
            + encoded_arguments
            + b"fplinux.kern-binary-sha256\0"
            + bytes.fromhex(lock["kern"]["binary_sha256"])
        ),
    )


def container_runtime_recipe_digest(image_recipe: str, image_generation: str) -> str:
    """Derive the exact runnable-image recipe from its static recipe and generation."""
    digest = hashlib.sha256()
    digest.update(b"fplinux.container-runtime-recipe\0")
    for item in (image_recipe, image_generation):
        _length_prefixed(digest, bytes.fromhex(item))
    return digest.hexdigest()


def container_image_reference(
    lock: dict[str, Any] | None = None, recipe: str | None = None
) -> str:
    """Return the recipe-addressed local reference for one build image."""
    if lock is None:
        lock = load_container_lock()
    if recipe is None:
        recipe = container_image_recipe_digest(lock)
    return f"{lock['oci']['repository']}:{recipe}"
