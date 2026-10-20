# SPDX-License-Identifier: GPL-2.0-only
"""Tests for exact host environment state cache lookups."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

from fplinux_cli.environment.state import (
    EnvironmentState,
    environment_state_path,
    load_environment_state,
    publish_environment_state,
)

if TYPE_CHECKING:
    from pathlib import Path


def state() -> EnvironmentState:
    """Return one stable state with distinct recipe, generation and content bytes."""
    return EnvironmentState("a" * 64, "b" * 64, "e" * 64)


class EnvironmentStateTests:
    """Use only an exact recipe and environment generation."""

    @staticmethod
    def test_exact_state_is_a_hit(tmp_path: Path) -> None:
        """Persist one exact environment generation at the fixed path."""
        cache = tmp_path / ".cache"
        expected = state()
        publish_environment_state(cache, expected)
        assert (load_environment_state(cache, "a" * 64)) == (expected)

    @staticmethod
    def test_recipe_or_generation_mismatch_is_a_miss(tmp_path: Path) -> None:
        """A different recipe or malformed generation cannot be reused."""
        cache = tmp_path / ".cache"
        expected = state()
        publish_environment_state(cache, expected)
        assert (load_environment_state(cache, "c" * 64)) is None
        path = environment_state_path(cache)
        payload = json.loads(path.read_text())
        payload["generation"] = "not-a-generation"
        path.write_text(json.dumps(payload), encoding="utf-8")
        assert (load_environment_state(cache, "a" * 64)) is None

    @staticmethod
    def test_invalid_json_is_a_miss(tmp_path: Path) -> None:
        """Unreadable state cache content cannot be reused."""
        cache = tmp_path / ".cache"
        path = environment_state_path(cache)
        path.parent.mkdir(parents=True)
        path.write_text("{", encoding="utf-8")
        assert (load_environment_state(cache, "a" * 64)) is None

    @staticmethod
    def test_publish_replaces_the_previous_state(tmp_path: Path) -> None:
        """A later environment state atomically supersedes the previous one."""
        cache = tmp_path / ".cache"
        publish_environment_state(cache, state())
        replacement = EnvironmentState("c" * 64, "d" * 64, "f" * 64)
        publish_environment_state(cache, replacement)
        assert (load_environment_state(cache, "a" * 64)) is None
        assert (load_environment_state(cache, "c" * 64)) == (replacement)
