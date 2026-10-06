# SPDX-License-Identifier: GPL-2.0-only
"""Tests for exact host image state cache lookups."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.environment.image_state import (
    ImageState,
    image_state_path,
    load_image_state,
    publish_image_state,
)

if TYPE_CHECKING:
    from pathlib import Path


def state() -> ImageState:
    """Return one stable state with distinct recipe and generation bytes."""
    return ImageState("a" * 64, "b" * 64, "b" * 64)


class ImageStateTests:
    """Use only an exact recipe and image generation."""

    @staticmethod
    def test_exact_state_is_a_hit(tmp_path: Path) -> None:
        """Persist one exact image generation at the fixed path."""
        cache = tmp_path / ".cache"
        expected = state()
        publish_image_state(cache, expected)
        assert (load_image_state(cache, "a" * 64)) == (expected)

    @staticmethod
    def test_recipe_or_generation_mismatch_is_a_miss(tmp_path: Path) -> None:
        """A different recipe or malformed generation cannot be reused."""
        cache = tmp_path / ".cache"
        expected = state()
        publish_image_state(cache, expected)
        assert (load_image_state(cache, "c" * 64)) is None
        path = image_state_path(cache)
        path.write_text(
            '{"container_image_recipe":"'
            + "a" * 64
            + '","image_generation":"not-a-generation"}\n',
            encoding="utf-8",
        )
        assert (load_image_state(cache, "a" * 64)) is None

    @staticmethod
    def test_invalid_json_is_a_miss(tmp_path: Path) -> None:
        """Unreadable state cache content cannot be reused."""
        cache = tmp_path / ".cache"
        cache.mkdir()
        image_state_path(cache).write_text("{", encoding="utf-8")
        assert (load_image_state(cache, "a" * 64)) is None

    @staticmethod
    def test_publish_replaces_the_previous_state(tmp_path: Path) -> None:
        """A later image state atomically supersedes the previous one."""
        cache = tmp_path / ".cache"
        publish_image_state(cache, state())
        replacement = ImageState("c" * 64, "d" * 64, "d" * 64)
        publish_image_state(cache, replacement)
        assert (load_image_state(cache, "a" * 64)) is None
        assert (load_image_state(cache, "c" * 64)) == (replacement)
