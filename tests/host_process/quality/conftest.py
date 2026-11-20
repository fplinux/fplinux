# SPDX-License-Identifier: GPL-2.0-only
"""Keep Git processes in the checkout created by each quality test."""

from __future__ import annotations

import os

import pytest

from tests.process import run_process


@pytest.fixture
def git_environment(monkeypatch: pytest.MonkeyPatch) -> dict[str, str]:
    """Remove checkout-specific overrides and ignore the user's Git configuration."""
    names = run_process(
        ["git", "rev-parse", "--local-env-vars"],
        name="list Git checkout environment variables",
        timeout=10,
        check=True,
    )
    for name in names.stdout.splitlines():
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", os.devnull)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    return os.environ.copy()
