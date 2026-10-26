# SPDX-License-Identifier: GPL-2.0-only
"""Offline export selection and documentation contents in synthetic host archives."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.packaging import documentation as package_docs

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
    """Content and notice edits reject old HTML; unrelated files retain the export."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    expected = write_offline_export(tmp_path)
    assert package_docs.offline_documentation() == expected
    (tmp_path / "unrelated.txt").write_text("Unrelated checkout input\n", encoding="utf-8")
    for directory in ("node_modules", "dist", ".astro"):
        ignored = tmp_path / "site" / directory
        ignored.mkdir()
        (ignored / "generated.txt").write_text("Build output\n", encoding="utf-8")
    assert package_docs.offline_documentation() == expected
    notices = tmp_path / "THIRD_PARTY_NOTICES.md"
    notices.write_bytes(b"Revised redistribution notice\n")
    with pytest.raises(SystemExit, match="documentation is stale"):
        package_docs.offline_documentation()
    expected = write_offline_export(tmp_path, notices=b"Revised redistribution notice\n")
    assert package_docs.offline_documentation() == expected
    (tmp_path / "site/src/content/docs/index.mdx").write_text(
        "# Revised documentation\n", encoding="utf-8"
    )
    with pytest.raises(SystemExit, match="documentation is stale"):
        package_docs.offline_documentation()
    revised = write_offline_export(tmp_path, text="Revised documentation")
    assert package_docs.offline_documentation() == revised


@pytest.mark.parametrize(
    "relative",
    [
        "targets/example/target.toml",
        "platforms/example/platform.toml",
        "targets/example/display/panel.toml",
    ],
)
def test_offline_export_rejects_changed_phone_inputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, relative: str
) -> None:
    """Identity, boot and panel sources cannot reuse HTML from their earlier bytes."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    phone_inputs = {
        "site/src/data/phones.json": b'[{"id":"example","slug":"phones/example"}]\n',
        "targets/example/target.toml": (
            b'platform = "example"\n[display]\npanel = "display/panel.toml"\n'
        ),
        "platforms/example/platform.toml": b'[identity]\nsoc = "EXAMPLE"\n',
        "targets/example/display/panel.toml": b"width = 123\nheight = 456\n",
    }
    expected = write_offline_export(tmp_path, phone_inputs=phone_inputs)
    assert package_docs.offline_documentation() == expected
    driver = tmp_path / "targets/example/linux/driver.c"
    driver.parent.mkdir(parents=True)
    driver.write_text("/* unrelated driver change */\n", encoding="utf-8")
    assert package_docs.offline_documentation() == expected
    source = tmp_path / relative
    source.write_bytes(source.read_bytes() + b"# revised phone input\n")
    with pytest.raises(SystemExit, match="documentation is stale"):
        package_docs.offline_documentation()
