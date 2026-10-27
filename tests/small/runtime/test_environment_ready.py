# SPDX-License-Identifier: GPL-2.0-only
"""Prepared environment admission without implicit setup or network access."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.environment import runtime, setup
from fplinux_cli.environment.state import EnvironmentState, publish_environment_state

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("target", "condition"),
    [
        pytest.param(None, "missing-runtime", id="missing-runtime"),
        pytest.param(None, "missing-receipt", id="missing-receipt"),
        pytest.param(None, "stale-recipe", id="stale-recipe"),
        pytest.param(None, "stale-content", id="stale-content"),
        pytest.param(None, "ready", id="ready"),
        pytest.param("example-phone", "missing-runtime", id="target-missing-runtime"),
    ],
)
def test_environment_admission_requires_exact_prepared_state_without_setup(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    condition: str,
    target: str | None,
) -> None:
    """A matching receipt and fresh content probe admit work; every miss directs setup."""
    state = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    cache = tmp_path / ".cache"
    if condition != "missing-receipt":
        recorded = (
            EnvironmentState("d" * 64, state.generation, state.content)
            if condition == "stale-recipe"
            else state
        )
        publish_environment_state(cache, recorded)
    monkeypatch.setattr(runtime, "ROOT", tmp_path)
    monkeypatch.setattr(runtime, "load_environment_lock", dict)
    monkeypatch.setattr(runtime, "environment_recipe_digest", lambda _lock: state.recipe)
    monkeypatch.setattr(runtime, "kern_available", lambda _lock: condition != "missing-runtime")
    monkeypatch.setattr(runtime, "require_kern", lambda _lock: "prepared-kern")
    monkeypatch.setattr(
        runtime,
        "probe_environment_state",
        lambda _kern, _rootfs, _recipe: None if condition == "stale-content" else state,
    )
    before = {
        path.relative_to(tmp_path): path.read_bytes()
        for path in cache.rglob("*")
        if path.is_file()
    }
    with (
        mock.patch.object(setup, "setup", side_effect=AssertionError("implicit setup")),
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
    ):
        if condition == "ready":
            assert runtime.require_environment(target=target) == (
                "prepared-kern",
                cache / "environment" / f"rootfs-{state.generation}",
                state,
            )
        else:
            selector = "--all" if target is None else "example-phone"
            with pytest.raises(
                SystemExit, match=rf"missing or stale; run ./fplinux setup {selector}$"
            ):
                runtime.require_environment(target=target)
    after = {
        path.relative_to(tmp_path): path.read_bytes()
        for path in cache.rglob("*")
        if path.is_file()
    }
    assert after == before
