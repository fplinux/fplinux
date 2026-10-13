# SPDX-License-Identifier: GPL-2.0-only
"""Offline export selection and documentation contents in synthetic host archives."""

from __future__ import annotations

import hashlib
import zipfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import documentation as package_docs

from tests.small.cli.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.small.cli.package_docs_fixtures import write_offline_export

if TYPE_CHECKING:
    from pathlib import Path

    from tests.small.cli.autonomous_package_fixtures import ArchiveInputs


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


def test_ordinary_linux_zip_contains_current_docs_without_changing_phone_qualification(
    archive_inputs: ArchiveInputs,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Synthetic builds keep their phone receipt when only generated docs change."""
    inputs = archive_inputs
    package.package_target(inputs.target, candidate=True)
    first_output = capsys.readouterr().out
    phone_digest = next(
        line.removeprefix("Phone-test payload SHA256: ")
        for line in first_output.splitlines()
        if line.startswith("Phone-test payload SHA256: ")
    )
    original = next((inputs.root / ".cache/out/candidates").glob("*.zip"))
    (inputs.root / "releases.lock.toml").write_text(
        f'[verified]\n{inputs.target} = "{phone_digest}"\n', encoding="utf-8"
    )
    expected = write_offline_export(inputs.root, text="Updated archive documentation")
    package.package_target(inputs.target, candidate=True)
    candidates = list((inputs.root / ".cache/out/candidates").glob("*.zip"))
    assert len(candidates) == 2
    assert (
        next(path for path in candidates if path != original).read_bytes() != original.read_bytes()
    )
    capsys.readouterr()
    package.package_target(inputs.target)
    assert f"Phone-test payload SHA256: {phone_digest}\n" in capsys.readouterr().out
    current = next((inputs.root / ".cache/out/releases").glob("*.zip"))
    assert current.name != original.name
    with zipfile.ZipFile(current) as archive:
        prefix = archive.namelist()[0].partition("/")[0] + "/"
        contents = {name.removeprefix(prefix): archive.read(name) for name in archive.namelist()}
        actual_docs = {name: data for name, data in contents.items() if name.startswith("docs/")}
        assert actual_docs == expected
        assert contents["image/ramboot.bin"] == (inputs.bundle / "image/ramboot.bin").read_bytes()
        assert not any(name.startswith(".cache/") for name in contents)
        assert "site-offline-receipt.json" not in contents
        checksums = {
            relative: digest
            for digest, relative in (
                line.split("  ", 1) for line in contents["SHA256SUMS"].decode().splitlines()
            )
        }
        for name, data in expected.items():
            assert checksums[name] == hashlib.sha256(data).hexdigest()
            assert archive.getinfo(prefix + name).external_attr >> 16 == 0o100644


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
