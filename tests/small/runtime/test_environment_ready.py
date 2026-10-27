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
    selected_root = tmp_path / ".data"
    if condition != "missing-receipt":
        recorded = (
            EnvironmentState("d" * 64, state.generation, state.content)
            if condition == "stale-recipe"
            else state
        )
        publish_environment_state(selected_root, recorded)
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(selected_root))
    monkeypatch.setattr(runtime, "ROOT", tmp_path)
    monkeypatch.setattr(runtime, "load_environment_lock", dict)
    monkeypatch.setattr(runtime, "environment_recipe_digest", lambda _lock: state.recipe)

    def kern_available(_lock: object, *, cache: Path) -> bool:
        assert cache == selected_root
        return condition != "missing-runtime"

    def require_kern(_lock: object, *, cache: Path) -> str:
        assert cache == selected_root
        return "prepared-kern"

    def probe_environment_state(
        kern: str, rootfs: Path, recipe: str, *, cache: Path
    ) -> EnvironmentState | None:
        assert cache == selected_root
        assert kern == "prepared-kern"
        assert rootfs == selected_root / "environment" / f"rootfs-{state.generation}"
        assert recipe == state.recipe
        return None if condition == "stale-content" else state

    monkeypatch.setattr(runtime, "kern_available", kern_available)
    monkeypatch.setattr(runtime, "require_kern", require_kern)
    monkeypatch.setattr(runtime, "probe_environment_state", probe_environment_state)
    before = {
        path.relative_to(tmp_path): path.read_bytes()
        for path in selected_root.rglob("*")
        if path.is_file()
    }
    with (
        mock.patch.object(setup, "setup", side_effect=AssertionError("implicit setup")),
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
    ):
        if condition == "ready":
            assert runtime.require_environment(target=target) == (
                "prepared-kern",
                selected_root / "environment" / f"rootfs-{state.generation}",
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
        for path in selected_root.rglob("*")
        if path.is_file()
    }
    assert after == before
