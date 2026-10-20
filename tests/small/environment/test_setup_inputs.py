# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for exact local setup inputs and private-cache exclusion."""

from __future__ import annotations

import hashlib
import io
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.environment import setup as env_setup

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def setup_inputs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, Path]:
    """Provide one verified archive and unrelated cache bytes without running installation."""
    root = tmp_path / "checkout"
    root.mkdir()
    context = tmp_path / "context"
    context.mkdir()
    source = root / ".cache/downloads/environment/tool/tool.tar.gz"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"exact source\n")
    private = root / ".cache/private/phone-state"
    private.parent.mkdir()
    private.write_bytes(b"private\n")
    recipe = root / "install.py"
    recipe.write_bytes(b"recipe fixture\n")
    recipe.chmod(0o755)
    item = DependencyInput(
        key="environment:tool",
        url="https://example.invalid/tool.tar.gz",
        sha256=hashlib.sha256(b"exact source\n").hexdigest(),
        size=13,
        destination="downloads/environment/tool/tool.tar.gz",
        purpose="environment-source",
    )
    monkeypatch.setattr(env_setup, "ROOT", root)
    monkeypatch.setattr(env_setup, "ENVIRONMENT_RECIPE_INPUTS", ("install.py",))
    monkeypatch.setattr(env_setup, "environment_inputs", lambda _root: [item])
    return root, context


def test_offline_staging_contains_declared_bytes_and_preserves_recipe_mode(
    setup_inputs: tuple[Path, Path],
) -> None:
    """Installation receives verified files and no unrelated cached phone state."""
    root, context = setup_inputs
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.
    assert (
        context / "inputs/downloads/environment/tool/tool.tar.gz"
    ).read_bytes() == b"exact source\n"
    assert (context / "install.py").read_bytes() == b"recipe fixture\n"
    assert (context / "install.py").stat().st_mode & 0o777 == 0o755
    assert not (context / "inputs/private").exists()
    assert (root / ".cache/private/phone-state").read_bytes() == b"private\n"


def test_offline_missing_input_reports_its_url_without_downloading(
    setup_inputs: tuple[Path, Path],
) -> None:
    """Missing original bytes stop before an incomplete installation can start."""
    root, context = setup_inputs
    (root / ".cache/downloads/environment/tool/tool.tar.gz").unlink()
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(
            SystemExit, match=r"offline locked input.*https://example.invalid/tool.tar.gz"
        ),
    ):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.
    assert not (context / "inputs/downloads/environment/tool/tool.tar.gz").exists()


def test_online_staging_downloads_and_checks_missing_original_bytes(
    setup_inputs: tuple[Path, Path],
) -> None:
    """The online host fetch supplies the same exact local input to the installer."""
    root, context = setup_inputs
    source = root / ".cache/downloads/environment/tool/tool.tar.gz"
    source.unlink()
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"exact source\n")):
        env_setup._stage_environment_inputs(context, offline=False)  # noqa: SLF001 -- test the host staging boundary.
    assert source.read_bytes() == b"exact source\n"
    assert (
        context / "inputs/downloads/environment/tool/tool.tar.gz"
    ).read_bytes() == b"exact source\n"


def test_mismatched_download_leaves_previous_cached_input(setup_inputs: tuple[Path, Path]) -> None:
    """A bad response cannot overwrite saved source bytes or reach installation."""
    root, context = setup_inputs
    source = root / ".cache/downloads/environment/tool/tool.tar.gz"
    source.write_bytes(b"previous cached source\n")
    with (
        mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"wrong source\n")),
        pytest.raises(SystemExit, match="locked download SHA256 mismatch"),
    ):
        env_setup._stage_environment_inputs(context, offline=False)  # noqa: SLF001 -- test the host staging boundary.
    assert source.read_bytes() == b"previous cached source\n"
    assert not (context / "inputs/downloads/environment/tool/tool.tar.gz").exists()
