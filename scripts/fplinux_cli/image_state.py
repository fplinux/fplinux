# SPDX-License-Identifier: GPL-2.0-only
"""Persist one exact immutable host image identity for check receipt lookup."""

from __future__ import annotations

import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path

IMAGE_STATE_NAME = "host-image-state.json"
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_IMAGE_ID = re.compile(r"sha256:[0-9a-f]{64}\Z")


class ImageStateError(ValueError):
    """Reject an unsafe host image state publication."""


def _require_digest(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ImageStateError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _require_image_identity(value: object) -> str:
    if not isinstance(value, str) or _IMAGE_ID.fullmatch(value) is None:
        message = "image identity must be an immutable sha256 image ID"
        raise ImageStateError(message)
    return value


@dataclass(frozen=True)
class ImageState:
    """The immutable OCI image that supplied one exact image recipe."""

    container_image_recipe: str
    image_identity: str

    def __post_init__(self) -> None:
        """Reject values that cannot identify an exact reusable image."""
        _require_digest(self.container_image_recipe, "container image recipe")
        _require_image_identity(self.image_identity)

    def payload(self) -> dict[str, str]:
        """Return the complete, fixed state payload."""
        return {
            "container_image_recipe": self.container_image_recipe,
            "image_identity": self.image_identity,
        }


def image_state_path(cache_root: Path) -> Path:
    """Return the fixed host image state path."""
    return Path(cache_root) / IMAGE_STATE_NAME


def load_image_state(cache_root: Path, image_recipe: str) -> ImageState | None:
    """Return an exact recipe match, or a cache miss."""
    try:
        image_recipe = _require_digest(image_recipe, "container image recipe")
        state = _read_state(image_state_path(Path(cache_root)))
        if state is None or state.container_image_recipe != image_recipe:
            return None
    except ImageStateError:
        return None
    else:
        return state


def publish_image_state(cache_root: Path, state: ImageState) -> Path:
    """Atomically replace the host image state."""
    if not isinstance(state, ImageState):
        message = "host image state is invalid"
        raise ImageStateError(message)
    directory = _ensure_cache_root(Path(cache_root))
    destination = image_state_path(directory)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=directory,
        prefix=f".{destination.stem}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(state.payload(), stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def _read_state(path: Path) -> ImageState | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or set(value) != {
            "container_image_recipe",
            "image_identity",
        }:
            return None
        return ImageState(
            container_image_recipe=value["container_image_recipe"],
            image_identity=value["image_identity"],
        )
    except (ImageStateError, OSError, ValueError):
        return None


def _ensure_cache_root(path: Path) -> Path:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        raise ImageStateError(f"host image cache root is missing or invalid: {path}")
    return path
