# SPDX-License-Identifier: GPL-2.0-only
"""Persist the current exact Kern image generation for cache receipt lookup."""

from __future__ import annotations

import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path

IMAGE_STATE_NAME = "host-image-state.json"
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class ImageStateError(ValueError):
    """Reject an unsafe host image state publication."""


def _require_digest(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ImageStateError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _require_image_generation(value: object) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        message = "image generation must be a lowercase SHA-256 digest"
        raise ImageStateError(message)
    return value


@dataclass(frozen=True)
class ImageState:
    """The exact Kern image generation built for one static image recipe."""

    container_image_recipe: str
    image_generation: str

    def __post_init__(self) -> None:
        """Reject values that cannot identify an exact reusable image."""
        _require_digest(self.container_image_recipe, "container image recipe")
        _require_image_generation(self.image_generation)

    def payload(self) -> dict[str, str]:
        """Return the complete, fixed state payload."""
        return {
            "container_image_recipe": self.container_image_recipe,
            "image_generation": self.image_generation,
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


def publish_image_state(cache_root: Path, state: ImageState) -> None:
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


def _read_state(path: Path) -> ImageState | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or set(value) != {
            "container_image_recipe",
            "image_generation",
        }:
            return None
        return ImageState(
            container_image_recipe=value["container_image_recipe"],
            image_generation=value["image_generation"],
        )
    except ImageStateError, OSError, ValueError:
        return None


def _ensure_cache_root(path: Path) -> Path:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        raise ImageStateError(f"host image cache root is missing or invalid: {path}")
    return path
