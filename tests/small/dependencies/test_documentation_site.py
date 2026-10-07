# SPDX-License-Identifier: GPL-2.0-only
"""Source-policy and formatter boundaries for the documentation website."""

from __future__ import annotations

from typing import TYPE_CHECKING

import check as source_check
import pytest
from fplinux_cli.quality.formatting.command import resolve_format_paths
from fplinux_cli.quality.formatting.source_formats import classify_source_formats
from fplinux_cli.quality.source_gate import validate_source_policy

if TYPE_CHECKING:
    from pathlib import Path


def write_files(root: Path, files: dict[str, bytes]) -> None:
    """Create test-owned files with their parent directories."""
    for relative, contents in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)


def test_site_images_and_font_do_not_enter_text_source_checks(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Keep bundled binary assets licensed while checking the website's text."""
    write_files(
        tmp_path,
        {
            "site/public/favicon.ico": b"\0\0\xff",
            "site/src/assets/screens/example.png": b"\x89PNG\0",
            "site/src/fonts/NotoSans.ttf": b"\0\x01\0",
            "site/src/fonts/OFL.txt": b"License text with its original spacing. \n",
            "site/src/assets/diagrams/example.svg": b"<svg/>\n",
            "site/src/content/docs/index.mdx": b"# Website\n",
        },
    )
    monkeypatch.setattr(source_check, "ROOT", tmp_path)

    files = source_check.source_files(enforce_policy=True)
    source_check.check_text(files)

    assert {path.relative_to(tmp_path).as_posix() for path in files} == {
        "site/src/assets/diagrams/example.svg",
        "site/src/content/docs/index.mdx",
    }


@pytest.mark.parametrize("path", ["site/tool.bin", "site/src/tool.png", "tool.ttf"])
def test_binary_artifacts_outside_site_asset_directories_are_rejected(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, path: str
) -> None:
    """Adding website assets does not allow executables or misplaced binary inputs."""
    write_files(tmp_path, {path: b"\0\xff"})
    monkeypatch.setattr(source_check, "ROOT", tmp_path)

    with pytest.raises(SystemExit, match="binary artifact is not allowed"):
        source_check.source_files(enforce_policy=True)


def test_root_formatter_preserves_the_separate_site_frontend_contract(tmp_path: Path) -> None:
    """Root documents and screenshot tools retain their existing formatter owner."""
    contents = {
        "docs/guide.md": b"# Guide\n",
        "site/src/content/docs/guide.md": b"# Website\n",
        "site/src/content/docs/guide.mdx": b"# Website\n",
        "site/src/components/Hero.astro": b"<main/>\n",
        "site/src/styles/custom.css": b"main {}\n",
        "site/package.json": b"{}\n",
        "site/config/prettier.config.mjs": b"export default {};\n",
        "site/src/data/support.json": b"{}\n",
        "site/scripts/screens/helper.py": b"print('screen')\n",
        "site/scripts/screens/render.sh": b"#!/bin/sh\necho screen\n",
    }
    write_files(tmp_path, contents)
    inventory = [(relative, tmp_path / relative) for relative in contents]

    formats = classify_source_formats([path for _, path in inventory], root=tmp_path)

    assert formats.markdown == ("docs/guide.md",)
    assert formats.python == ("site/scripts/screens/helper.py",)
    assert formats.posix_shell == ("site/scripts/screens/render.sh",)
    root_owned = {
        "docs/guide.md",
        "site/scripts/screens/helper.py",
        "site/scripts/screens/render.sh",
    }
    assert formats.supported() == root_owned
    for relative in contents:
        if relative in root_owned:
            resolve_format_paths([relative], root=tmp_path, inventory=inventory)
        else:
            with pytest.raises(SystemExit, match="no project formatter is defined"):
                resolve_format_paths([relative], root=tmp_path, inventory=inventory)


def test_installed_site_packages_and_generated_output_are_not_container_sources(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A site install cannot introduce a second first-party build-image recipe."""
    write_files(
        tmp_path,
        {
            "Containerfile": b"ARG BASE_IMAGE\nFROM ${BASE_IMAGE}\n",
            "site/node_modules/package/Containerfile": b"FROM external\n",
            "site/.astro/Containerfile": b"FROM generated\n",
            "site/dist/Containerfile": b"FROM output\n",
        },
    )
    monkeypatch.setattr("fplinux_cli.common.ROOT", tmp_path)

    validate_source_policy()

    write_files(tmp_path, {"site/Containerfile": b"FROM other\n"})
    with pytest.raises(SystemExit, match="exactly one root Containerfile"):
        validate_source_policy()
