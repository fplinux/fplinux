# SPDX-License-Identifier: GPL-2.0-only
"""Read the public data path before any command backend or target discovery exists."""

from __future__ import annotations

import os
import shutil
import subprocess
from typing import TYPE_CHECKING

import pytest

from tests.cli_support import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def path_only_installation(tmp_path: Path) -> Path:
    """Provide only the launcher, entrypoint and pure resolver, without domain modules."""
    installation = tmp_path / "installation"
    package = installation / "scripts/fplinux_cli"
    package.mkdir(parents=True)
    shutil.copy(ROOT / "fplinux", installation / "fplinux")
    for name in ("__init__.py", "__main__.py", "data_paths.py"):
        shutil.copy(ROOT / "scripts/fplinux_cli" / name, package / name)
    return installation


@pytest.fixture
def checked_path_installation(path_only_installation: Path) -> Path:
    """Add placement validation without command backends or target manifests."""
    for name in ("common.py", "source_paths.py"):
        shutil.copy(
            ROOT / "scripts/fplinux_cli" / name,
            path_only_installation / "scripts/fplinux_cli" / name,
        )
    return path_only_installation


@pytest.mark.parametrize("selection", [None, "space and Юникод", "~/$DATA/%USER%/*"])
def test_public_data_path_is_installation_relative_from_another_cwd_and_creates_nothing(
    path_only_installation: Path, tmp_path: Path, selection: str | None
) -> None:
    environment = os.environ.copy()
    environment.pop("FPLINUX_DATA_DIR", None)
    if selection is not None:
        environment["FPLINUX_DATA_DIR"] = selection
    before = set(path_only_installation.rglob("*"))

    result = run_process(
        [str(path_only_installation / "fplinux"), "data-path"],
        name="read-only data path",
        cwd=tmp_path,
        env=environment,
        timeout=10,
    )

    expected = path_only_installation / (".data" if selection is None else selection)
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{expected}\n"
    assert result.stderr == ""
    assert set(path_only_installation.rglob("*")) == before


def test_public_data_path_accepts_an_absolute_external_directory_without_creating_it(
    path_only_installation: Path, tmp_path: Path
) -> None:
    external = tmp_path / "external space/данные"

    result = run_process(
        [str(path_only_installation / "fplinux"), "data-path"],
        name="external data path",
        cwd=tmp_path,
        env={**os.environ, "FPLINUX_DATA_DIR": str(external)},
        timeout=10,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{external}\n"
    assert result.stderr == ""
    assert not external.exists()


@pytest.mark.parametrize("value", ["", "bad\npath", "bad\rpath"])
def test_public_data_path_reports_invalid_values_without_importing_command_backends(
    path_only_installation: Path, value: str
) -> None:
    result = run_process(
        [str(path_only_installation / "fplinux"), "data-path"],
        name="invalid data path",
        cwd=path_only_installation,
        env={**os.environ, "FPLINUX_DATA_DIR": value},
        timeout=10,
    )

    assert result.returncode == 1
    assert result.stdout == ""
    assert "FPLINUX_DATA_DIR" in result.stderr
    assert "Traceback" not in result.stderr
    assert not (path_only_installation / ".data").exists()


def test_checked_data_path_rejects_tracked_source_placement_without_creating_state(
    checked_path_installation: Path,
) -> None:
    installation = checked_path_installation
    source = installation / "source-inputs/index.txt"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"source input\n")
    subprocess.run(["git", "init", "--quiet", str(installation)], check=True, capture_output=True)
    subprocess.run(
        ["git", "-C", str(installation), "add", "source-inputs/index.txt"],
        check=True,
        capture_output=True,
    )
    environment = {**os.environ, "FPLINUX_DATA_DIR": "source-inputs"}

    pure = run_process(
        [str(installation / "fplinux"), "data-path"],
        name="unchecked data path",
        cwd=installation,
        env=environment,
        timeout=10,
    )
    checked = run_process(
        [str(installation / "fplinux"), "data-path", "--check"],
        name="checked data path",
        cwd=installation,
        env=environment,
        timeout=10,
    )

    assert pure.returncode == 0, pure.stderr
    assert pure.stdout == f"{installation / 'source-inputs'}\n"
    assert checked.returncode == 1
    assert checked.stdout == ""
    assert "overlaps tracked source" in checked.stderr
    assert source.read_bytes() == b"source input\n"
    assert not (installation / ".data").exists()


@pytest.mark.parametrize(
    "selection", ["site", "site/working-data", "site/src/content/docs/state", "site/public/state"]
)
def test_checked_data_path_rejects_native_site_inputs_before_creating_state(
    checked_path_installation: Path, selection: str
) -> None:
    """Source exports also reject storage that native website tools would consume."""
    installation = checked_path_installation
    before = set(installation.rglob("*"))
    environment = {**os.environ, "FPLINUX_DATA_DIR": selection}
    pure = run_process(
        [str(installation / "fplinux"), "data-path"],
        name="unchecked website data path",
        cwd=installation,
        env=environment,
        timeout=10,
    )
    result = run_process(
        [str(installation / "fplinux"), "data-path", "--check"],
        name="website data placement",
        cwd=installation,
        env=environment,
        timeout=10,
    )

    assert pure.returncode == 0, pure.stderr
    assert pure.stdout == f"{installation / selection}\n"
    assert result.returncode == 1
    assert result.stdout == ""
    assert "must not be inside the website source tree" in result.stderr
    assert "Traceback" not in result.stderr
    assert set(installation.rglob("*")) == before


def test_checked_data_path_accepts_a_website_sibling_without_creating_state(
    checked_path_installation: Path,
) -> None:
    """The website boundary is a directory, not a matching pathname prefix."""
    installation = checked_path_installation
    result = run_process(
        [str(installation / "fplinux"), "data-path", "--check"],
        name="website sibling data placement",
        cwd=installation,
        env={**os.environ, "FPLINUX_DATA_DIR": "site-data"},
        timeout=10,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{installation / 'site-data'}\n"
    assert not (installation / "site-data").exists()
