# SPDX-License-Identifier: GPL-2.0-only
"""Admission boundaries for source, selected data and disposable snapshots."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.quality import source as source_check
from fplinux_cli.source_paths import source_path_excluded, validate_checkout_data_root
from fplinux_cli.workspace import build_inputs, quality_inputs, staging
from fplinux_cli.workspace.capture import workspace_snapshot

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize("relative", [".data", ".private", "scripts/generated"])
def test_source_inventories_skip_managed_and_private_trees_before_admission(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, relative: str
) -> None:
    """Generated symlinks cannot fail checks or enter either source closure."""
    source = tmp_path / "scripts/main.py"
    source.parent.mkdir()
    source.write_bytes(b"print('source')\n")
    excluded = tmp_path / relative
    excluded.mkdir(parents=True, exist_ok=True)
    (excluded / "linked.py").symlink_to(source)
    selected = tmp_path / "scripts/generated"
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(selected))
    for module in (source_check, build_inputs, quality_inputs):
        monkeypatch.setattr(module, "ROOT", tmp_path)
    inventory = subprocess.CompletedProcess(
        ["git", "ls-files"], 0, f"scripts/main.py\0{relative}/linked.py\0".encode(), b""
    )
    monkeypatch.setattr(subprocess, "run", lambda *_args, **_kwargs: inventory)

    assert source_check.source_files(enforce_policy=True) == [source]
    assert quality_inputs.quality_files(enforce_source_policy=True) == [
        ("scripts/main.py", source)
    ]
    files: dict[str, Path] = {}
    build_inputs.add_source_path(files, tmp_path / "scripts")
    assert files == {"scripts/main.py": source}


@pytest.mark.parametrize(
    "relative", [".data/input.py", ".private/input.py", "scripts/generated/input.py"]
)
def test_required_build_source_inside_excluded_tree_reports_an_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, relative: str
) -> None:
    """A required declaration must not silently produce an incomplete build closure."""
    monkeypatch.setattr(build_inputs, "ROOT", tmp_path)
    monkeypatch.setenv("FPLINUX_DATA_DIR", "scripts/generated")
    with pytest.raises(SystemExit, match="required workspace input is inside an excluded"):
        build_inputs.add_source_path({}, tmp_path / relative)


def test_snapshot_under_data_root_remains_a_readable_source_tree(tmp_path: Path) -> None:
    """Only a data subtree inside the current scan root is excluded."""
    selected = tmp_path / "external data"
    snapshot = selected / "workspaces/captured"
    assert not source_path_excluded(
        snapshot / "scripts/main.py", source_root=snapshot, selected_data_root=selected
    )
    assert source_path_excluded(
        snapshot / ".private/note.py", source_root=snapshot, selected_data_root=selected
    )
    assert source_path_excluded(
        snapshot / ".data/generated.py", source_root=snapshot, selected_data_root=selected
    )


@pytest.mark.parametrize(
    "placement", ["checkout", ".", "checkout/.private", "checkout/.private/nested"]
)
def test_checkout_placement_rejects_source_ancestors_and_private_material(
    tmp_path: Path, placement: str
) -> None:
    """Placement is checked before a writer can manage source or local evidence."""
    source = tmp_path / "checkout"
    with pytest.raises(SystemExit, match="managed data root must not"):
        validate_checkout_data_root(source, tmp_path / placement)


@pytest.mark.parametrize(
    "relative", ["site", "site/generated", "site/src/state", "site/public/state"]
)
def test_checkout_placement_keeps_managed_storage_outside_native_site_inputs(
    tmp_path: Path, relative: str
) -> None:
    """Native website discovery cannot admit an installation's working storage."""
    with pytest.raises(SystemExit, match="must not be inside the website source tree"):
        validate_checkout_data_root(tmp_path, tmp_path / relative)


def test_checkout_placement_preserves_script_subtrees_and_website_siblings(tmp_path: Path) -> None:
    """Managed paths handled by source admission remain available outside the website."""
    validate_checkout_data_root(tmp_path, tmp_path / "scripts/generated")
    validate_checkout_data_root(tmp_path, tmp_path / "site-data")


def test_workspace_discard_uses_its_captured_root_after_environment_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An ordinary environment or cwd change cannot move cleanup to another store."""
    source = tmp_path / "source.py"
    source.write_bytes(b"captured\n")
    snapshot = workspace_snapshot([("source.py", source)])
    selected = tmp_path / "selected data"
    monkeypatch.setattr(staging, "ROOT", tmp_path)
    workspace = staging.stage_quality_workspace_snapshot(snapshot, selected_data_root=selected)
    monkeypatch.setenv("FPLINUX_DATA_DIR", "different-store")
    monkeypatch.chdir(tmp_path.parent)
    staging.discard_staged_quality_workspace_snapshot(
        snapshot, workspace, selected_data_root=selected
    )
    assert not workspace.exists()
    assert not (tmp_path / "different-store").exists()
