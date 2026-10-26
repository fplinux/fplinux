# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for exact local setup inputs and private-cache exclusion."""

from __future__ import annotations

import hashlib
import io
import json
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.dependencies.inputs import (
    DependencyInput,
    terminus_font_environment_inputs,
    ui_font_environment_inputs,
    ui_icon_environment_inputs,
)
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
    for package, declarations in (
        ("fplinux-ui", "face ofl"),
        ("fplinux-font-terminus", "terminus"),
        ("fplinux-icons", "tabler"),
    ):
        aport = root / "alpine/aports" / package / "APKBUILD"
        aport.parent.mkdir(parents=True)
        references = " ".join(f"@source:{name}@" for name in declarations.split())
        aport.write_text(f'pkgname={package}\npkgver=1\nsource="{references}"\nsha512sums=""\n')
    catalog = []
    for name, filename, data in (
        ("face", "face.ttf", b"font fixture\n"),
        ("ofl", "OFL.txt", b"font license\n"),
        ("terminus", "terminus.tar.gz", b"bitmap archive fixture\n"),
        ("tabler", "tabler.tgz", b"icon archive fixture\n"),
    ):
        original = root / ".cache/downloads/sources" / name / filename
        original.parent.mkdir(parents=True)
        original.write_bytes(data)
        catalog.append(
            f'[{name}]\nversion="1"\nname="{filename}"\nurl="https://example.invalid/{filename}"\nalgorithm="sha512"\nchecksum="{hashlib.sha512(data).hexdigest()}"\n'
        )
    (root / "sources.lock.toml").write_text("\n".join(catalog))
    (root / "package-lock.json").write_text(
        json.dumps({"packages": {"node_modules/lv_font_conv": {"version": "1.5.3"}}})
    )
    monkeypatch.setattr(
        env_setup,
        "environment_inputs",
        lambda selected_root: [
            item,
            *ui_font_environment_inputs(selected_root),
            *terminus_font_environment_inputs(selected_root),
            *ui_icon_environment_inputs(selected_root),
        ],
    )
    monkeypatch.setattr(env_setup, "load_environment_lock", lambda: {"input": []})
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


def test_offline_staging_captures_the_icon_packages_declared_archive(
    setup_inputs: tuple[Path, Path],
) -> None:
    """The same verified package original reaches the offline native renderer."""
    _root, context = setup_inputs
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.
    assert (context / "quality/ui-icons/tabler.tgz").read_bytes() == b"icon archive fixture\n"
    assert not (context / "inputs/downloads/sources/tabler/tabler.tgz").exists()


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


@pytest.fixture
def ui_font_inputs(setup_inputs: tuple[Path, Path]) -> tuple[Path, Path]:
    """Declare font originals in their package and isolate setup from unrelated downloads."""
    root, context = setup_inputs
    return root, context


def test_offline_staging_captures_ui_font_and_license_originals(
    ui_font_inputs: tuple[Path, Path],
) -> None:
    """Offline rendering receives exact package originals without unrelated aport files."""
    _root, context = ui_font_inputs
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.
    assert (context / "quality/ui-fonts/face.ttf").read_bytes() == b"font fixture\n"
    assert (context / "quality/ui-fonts/OFL.txt").read_bytes() == b"font license\n"
    assert not (context / "quality/ui-fonts/local.c").exists()


@pytest.mark.parametrize("available", [False, True], ids=("missing", "wrong-checksum"))
def test_offline_staging_rejects_unavailable_ui_font_original(
    ui_font_inputs: tuple[Path, Path], *, available: bool
) -> None:
    """Missing or mismatched originals stop setup instead of silently omitting UI fonts."""
    root, context = ui_font_inputs
    font = root / ".cache/downloads/sources/face/face.ttf"
    if available:
        font.write_bytes(b"different font\n")
    else:
        font.unlink()
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(SystemExit, match=r"offline locked input.*https://example.invalid/face.ttf"),
    ):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.
    assert not (context / "quality/ui-fonts/face.ttf").exists()


@pytest.fixture
def terminal_font_inputs(setup_inputs: tuple[Path, Path]) -> tuple[Path, Path]:
    """Admit the bitmap archive through its package checksum declaration."""
    root, context = setup_inputs
    archive = root / ".cache/downloads/sources/terminus/terminus.tar.gz"
    archive.with_name("unrelated.tar.gz").write_bytes(b"unrelated source\n")
    return root, context


def test_offline_staging_captures_exact_terminal_font_archive(
    terminal_font_inputs: tuple[Path, Path],
) -> None:
    """The installer receives its verified archive without unrelated source-cache bytes."""
    _root, context = terminal_font_inputs
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.

    assert (
        context / "quality/terminus/terminus.tar.gz"
    ).read_bytes() == b"bitmap archive fixture\n"
    assert not (context / "quality/terminus/unrelated.tar.gz").exists()


@pytest.mark.parametrize("available", [False, True], ids=("missing", "wrong-checksum"))
def test_offline_staging_rejects_unavailable_terminal_font_archive(
    terminal_font_inputs: tuple[Path, Path], *, available: bool
) -> None:
    """A missing or mismatched font archive cannot reach offline installation."""
    root, context = terminal_font_inputs
    archive = root / ".cache/downloads/sources/terminus/terminus.tar.gz"
    if available:
        archive.write_bytes(b"different archive\n")
    else:
        archive.unlink()
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(
            SystemExit, match=r"offline locked input.*https://example.invalid/terminus.tar.gz"
        ),
    ):
        env_setup._stage_environment_inputs(context, offline=True)  # noqa: SLF001 -- test the host staging boundary.

    assert not (context / "quality/terminus/terminus.tar.gz").exists()
