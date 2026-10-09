# SPDX-License-Identifier: GPL-2.0-only
"""Offline export selection and documentation contents in synthetic host archives."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.cli import package_docs

from tests.small.cli.package_docs_fixtures import write_offline_export

if TYPE_CHECKING:
    from pathlib import Path


def test_offline_export_requires_a_completed_build(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Missing generated documentation tells the caller how to prepare it."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    with pytest.raises(SystemExit, match="run npm run build:offline in site"):
        package_docs.offline_documentation()


def test_offline_export_tracks_site_inputs_and_ignores_unrelated_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A content edit rejects old HTML, while unrelated checkout and build files do not."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    expected = write_offline_export(tmp_path)
    assert package_docs.offline_documentation() == expected
    (tmp_path / "unrelated.txt").write_text("Unrelated checkout input\n", encoding="utf-8")
    for directory in ("node_modules", "dist", ".astro"):
        ignored = tmp_path / "site" / directory
        ignored.mkdir()
        (ignored / "generated.txt").write_text("Build output\n", encoding="utf-8")
    assert package_docs.offline_documentation() == expected
    (tmp_path / "site/src/content/docs/index.mdx").write_text(
        "# Revised documentation\n", encoding="utf-8"
    )
    with pytest.raises(SystemExit, match="documentation is stale"):
        package_docs.offline_documentation()
    revised = write_offline_export(tmp_path, text="Revised documentation")
    assert package_docs.offline_documentation() == revised
