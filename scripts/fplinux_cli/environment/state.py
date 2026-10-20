# SPDX-License-Identifier: GPL-2.0-only
"""Persist the prepared environment's recipe, generation and installed content."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from fplinux_cli.common import canonical_json_bytes, replace_file_atomically

ENVIRONMENT_STATE_NAME = "state.json"
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class EnvironmentStateError(ValueError):
    """Reject an invalid environment state publication."""


def _require_digest(value: object, field: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise EnvironmentStateError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _require_generation(value: object) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        message = "environment generation must be a lowercase SHA-256 digest"
        raise EnvironmentStateError(message)
    return value


@dataclass(frozen=True)
class EnvironmentState:
    """Operational generation and actual content prepared for one declared recipe."""

    recipe: str
    generation: str
    content: str

    def __post_init__(self) -> None:
        """Reject values that cannot identify an exact reusable environment."""
        _require_digest(self.recipe, "environment recipe")
        _require_generation(self.generation)
        _require_digest(self.content, "environment content")

    def payload(self) -> dict[str, str]:
        """Return the complete, fixed state payload."""
        return {
            "recipe": self.recipe,
            "generation": self.generation,
            "content": self.content,
        }


def environment_state_path(cache_root: Path) -> Path:
    """Return the fixed prepared-environment receipt path."""
    return Path(cache_root) / "environment" / ENVIRONMENT_STATE_NAME


def load_environment_state(cache_root: Path, recipe: str) -> EnvironmentState | None:
    """Return an exact recipe match, or a cache miss."""
    try:
        recipe = _require_digest(recipe, "environment recipe")
        state = _read_state(environment_state_path(Path(cache_root)))
        if state is None or state.recipe != recipe:
            return None
    except EnvironmentStateError:
        return None
    else:
        return state


def publish_environment_state(cache_root: Path, state: EnvironmentState) -> None:
    """Atomically replace the prepared-environment receipt."""
    if not isinstance(state, EnvironmentState):
        message = "host environment state is invalid"
        raise EnvironmentStateError(message)
    destination = environment_state_path(Path(cache_root))
    _ensure_cache_root(destination.parent)
    replace_file_atomically(destination, canonical_json_bytes(state.payload()), 0o600, sync=False)


def _read_state(path: Path) -> EnvironmentState | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or set(value) != {
            "recipe",
            "generation",
            "content",
        }:
            return None
        return EnvironmentState(
            recipe=value["recipe"],
            generation=value["generation"],
            content=value["content"],
        )
    except EnvironmentStateError, OSError, ValueError:
        return None


def _ensure_cache_root(path: Path) -> Path:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        raise EnvironmentStateError(f"host environment cache root is missing or invalid: {path}")
    return path
