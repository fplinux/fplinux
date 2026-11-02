# SPDX-License-Identifier: GPL-2.0-only
"""Installation-relative managed storage without setup or filesystem writes."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.data_paths import data_root

if TYPE_CHECKING:
    from pathlib import Path


def test_default_data_root_is_beside_the_installation_without_creating_it(tmp_path: Path) -> None:
    installation = tmp_path / "installation"

    selected = data_root(installation, environment={})

    assert selected == tmp_path / "installation/.data"
    assert not installation.exists()


@pytest.mark.parametrize("value", ["state", "space and Юникод", "~/$DATA/%USER%/*"])
def test_relative_data_root_uses_the_installation_without_shell_expansion(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, value: str
) -> None:
    installation = tmp_path / "installation"
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)

    selected = data_root(installation, environment={"FPLINUX_DATA_DIR": value})

    assert selected == installation / value
    assert not installation.exists()


def test_absolute_data_root_ignores_the_installation_and_normalizes_parent_components(
    tmp_path: Path,
) -> None:
    selected = data_root(
        tmp_path / "installation",
        environment={"FPLINUX_DATA_DIR": str(tmp_path / "external/../selected")},
    )

    assert selected == tmp_path / "selected"
    assert not selected.exists()


@pytest.mark.parametrize("value", ["", "bad\npath", "bad\rpath", "bad\0path"])
def test_invalid_data_root_is_rejected_without_creating_an_installation(
    tmp_path: Path, value: str
) -> None:
    installation = tmp_path / "installation"

    with pytest.raises(ValueError, match="FPLINUX_DATA_DIR"):
        data_root(installation, environment={"FPLINUX_DATA_DIR": value})

    assert not installation.exists()


@pytest.mark.skipif(os.name != "nt", reason="native Windows path policy")
@pytest.mark.parametrize("value", ["D:state", "\\state"])
def test_native_windows_ambiguous_data_root_is_rejected(tmp_path: Path, value: str) -> None:
    with pytest.raises(ValueError, match="fully qualified"):
        data_root(tmp_path, environment={"FPLINUX_DATA_DIR": value})
