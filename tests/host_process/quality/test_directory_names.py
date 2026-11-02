# SPDX-License-Identifier: GPL-2.0-only
"""Check directory policy against Git-backed source inventories."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.workspace import quality_inputs

from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_scoped_inventory_bypasses_directory_policy_and_source_checks_binary_resources(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The source-wide gate includes resources and excludes ignored local files."""
    run_process(["git", "init", "--quiet", str(tmp_path)], name="source Git setup", timeout=10)
    (tmp_path / ".gitignore").write_text(".data/\nignored_dir/\n", encoding="utf-8")
    for directory in ("resource_names", ".data", "ignored_dir"):
        path = tmp_path / directory / "image.png"
        path.parent.mkdir()
        path.write_bytes(b"\x89PNG\x00\xff")
    monkeypatch.setattr(quality_inputs, "ROOT", tmp_path)

    assert quality_inputs.quality_files(enforce_source_policy=False) == [
        (".gitignore", tmp_path / ".gitignore"),
        ("resource_names/image.png", tmp_path / "resource_names/image.png"),
    ]
    with pytest.raises(
        SystemExit,
        match=r"^fplinux: source directory must use kebab-case: resource_names$",
    ):
        quality_inputs.quality_files(enforce_source_policy=True)


def test_deleted_package_marker_does_not_exempt_remaining_resources(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A tracked deletion changes package classification to the current resource inventory."""
    run_process(["git", "init", "--quiet", str(tmp_path)], name="source Git setup", timeout=10)
    package = tmp_path / "phone_tools"
    package.mkdir()
    initializer = package / "__init__.py"
    initializer.touch()
    resource = package / "image.png"
    resource.write_bytes(b"\x89PNG\x00\xff")
    run_process(
        ["git", "-C", str(tmp_path), "add", "phone_tools/__init__.py"],
        name="source package marker setup",
        timeout=10,
    )
    monkeypatch.setattr(quality_inputs, "ROOT", tmp_path)
    assert quality_inputs.quality_files(enforce_source_policy=True) == [
        ("phone_tools/__init__.py", initializer),
        ("phone_tools/image.png", resource),
    ]

    initializer.unlink()

    with pytest.raises(
        SystemExit,
        match=r"^fplinux: source directory must use kebab-case: phone_tools$",
    ):
        quality_inputs.quality_files(enforce_source_policy=True)
