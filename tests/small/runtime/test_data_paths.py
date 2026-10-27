# SPDX-License-Identifier: GPL-2.0-only
"""Protect installation scopes and delivered runtime path overrides."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime import cli
from fplinux_cli.runtime.data_paths import (
    archive_runtime_root,
    checkout_runtime_root,
    installation_id,
)

if TYPE_CHECKING:
    from collections.abc import Callable


@pytest.mark.skipif(os.name != "posix", reason="literal canonical POSIX path")
def test_installation_id_hashes_the_canonical_native_installation_path() -> None:
    assert installation_id(Path("/opt/fplinux")) == (
        "f2e57ddb99d9641b49b18ca1d9e31e233bb859a6550c162df0fef4df7af91cc1"
    )


def test_runtime_scopes_separate_installations_deliveries_and_targets(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = tmp_path / "first installation"
    second = tmp_path / "вторая установка"
    managed = tmp_path / "shared data"
    original = archive_runtime_root(managed, first)
    monkeypatch.chdir(tmp_path.parent)

    assert archive_runtime_root(managed, first / "child/..") == original
    assert archive_runtime_root(managed, second) != original
    checkout = checkout_runtime_root(managed, first, "phone")
    assert checkout.parent.parent == managed / "runtime/checkouts"
    assert checkout.name == "phone"
    assert checkout != original
    assert checkout_runtime_root(managed, second, "phone") != checkout
    assert checkout_runtime_root(managed, first, "other-phone") != checkout
    assert original.parent == managed / "runtime/archives"
    assert not managed.exists()


@pytest.fixture
def selected_runtime(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> tuple[argparse.Namespace, Callable[[], tuple[Path, Path, Path]]]:
    """Replace the phone boundary while retaining the delivered path selection."""
    options = argparse.Namespace(
        bundle=tmp_path / "selected bundle",
        data_root=None,
        log_root=None,
        phone_data=None,
        command="prepare",
    )
    observed: list[tuple[Path, Path, Path]] = []

    def phone(bundle: Path, data: Path, logs: Path, **_options: object) -> int:
        observed.append((bundle, data, logs))
        return 0

    def select() -> tuple[Path, Path, Path]:
        with pytest.raises(SystemExit) as result:
            cli.main(bundle_root=tmp_path / "entry bundle")
        assert result.value.code == 0
        return observed[-1]

    monkeypatch.setattr(cli, "arguments", lambda _root: options)
    monkeypatch.setattr(cli, "run_phone", phone)
    return options, select


def test_archive_environment_is_relative_to_the_selected_bundle_from_other_cwd(
    selected_runtime: tuple[argparse.Namespace, Callable[[], tuple[Path, Path, Path]]],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    options, select = selected_runtime
    monkeypatch.setenv("FPLINUX_DATA_DIR", "данные телефона")
    monkeypatch.chdir(tmp_path.parent)

    bundle, data, logs = select()

    assert bundle == options.bundle
    assert data.parent.parent.parent == bundle / "данные телефона"
    assert data.parent.name == "archives"
    assert logs == data / "logs"
    assert not data.exists()


def test_explicit_runtime_paths_keep_cwd_semantics_and_override_invalid_environment(
    selected_runtime: tuple[argparse.Namespace, Callable[[], tuple[Path, Path, Path]]],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    options, select = selected_runtime
    options.data_root = Path("selected state")
    options.log_root = Path("selected logs")
    monkeypatch.setenv("FPLINUX_DATA_DIR", "")
    monkeypatch.chdir(tmp_path)

    _bundle, data, logs = select()

    assert data == tmp_path / "selected state"
    assert logs == tmp_path / "selected logs"
    assert not data.exists()


@pytest.mark.parametrize("override", ["data_root", "log_root"])
def test_one_explicit_runtime_path_preserves_the_other_managed_default(
    selected_runtime: tuple[argparse.Namespace, Callable[[], tuple[Path, Path, Path]]],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    override: str,
) -> None:
    options, select = selected_runtime
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(tmp_path / "external data"))
    monkeypatch.chdir(tmp_path)
    setattr(options, override, Path("explicit path"))

    _bundle, data, logs = select()

    if override == "data_root":
        assert data == tmp_path / "explicit path"
        assert logs.parent.parent.parent.parent == tmp_path / "external data"
    else:
        assert data.parent.parent.parent == tmp_path / "external data"
        assert logs == tmp_path / "explicit path"


@pytest.mark.usefixtures("selected_runtime")
def test_invalid_archive_data_environment_fails_before_the_phone_boundary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("FPLINUX_DATA_DIR", "")

    with pytest.raises(SystemExit, match="FPLINUX_DATA_DIR must be a non-empty path"):
        cli.main(bundle_root=tmp_path)

    assert not (tmp_path / ".data").exists()
