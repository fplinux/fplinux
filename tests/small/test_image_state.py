# SPDX-License-Identifier: GPL-2.0-only
"""Tests for exact host image state cache lookups."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.image_state import (
    ImageState,
    image_state_path,
    load_image_state,
    publish_image_state,
)


def state() -> ImageState:
    """Return one stable state with distinct recipe and generation bytes."""
    return ImageState("a" * 64, "b" * 64)


class ImageStateTests(unittest.TestCase):
    """Use only an exact recipe and image generation."""

    def test_exact_state_is_a_hit(self) -> None:
        """Persist one exact image generation at the fixed path."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = state()
            publish_image_state(cache, expected)
            self.assertEqual(load_image_state(cache, "a" * 64), expected)

    def test_recipe_or_generation_mismatch_is_a_miss(self) -> None:
        """A different recipe or malformed generation cannot be reused."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = state()
            publish_image_state(cache, expected)
            self.assertIsNone(load_image_state(cache, "c" * 64))
            path = image_state_path(cache)
            path.write_text(
                '{"container_image_recipe":"'
                + "a" * 64
                + '","image_generation":"not-a-generation"}\n',
                encoding="utf-8",
            )
            self.assertIsNone(load_image_state(cache, "a" * 64))

    def test_invalid_json_is_a_miss(self) -> None:
        """Unreadable state cache content cannot be reused."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            cache.mkdir()
            image_state_path(cache).write_text("{", encoding="utf-8")
            self.assertIsNone(load_image_state(cache, "a" * 64))

    def test_publish_replaces_the_previous_state(self) -> None:
        """A later image state atomically supersedes the previous one."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            publish_image_state(cache, state())
            replacement = ImageState("c" * 64, "d" * 64)
            publish_image_state(cache, replacement)
            self.assertIsNone(load_image_state(cache, "a" * 64))
            self.assertEqual(load_image_state(cache, "c" * 64), replacement)


if __name__ == "__main__":
    unittest.main()
