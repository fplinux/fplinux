# SPDX-License-Identifier: GPL-2.0-only
"""Inspect the final release association and its actual ZIP and transport bytes."""

from __future__ import annotations

import hashlib
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cli.inspect import inspect_archive, inspect_distribution
from fplinux_cli.packaging import distribution

from tests.distribution_support import (
    RELEASE_ID,
    SELECTIONS,
    read_zip,
    write_pair,
    write_transport,
    write_zip,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def assets(tmp_path: Path) -> Path:
    packages = tmp_path / "packages"
    packages.mkdir()
    for target, host in SELECTIONS:
        write_pair(packages, target, host)
    transport = tmp_path / "transport"
    write_transport(transport)
    output = tmp_path / "final"
    distribution.assemble_distribution(packages, output, inputs_directory=transport)
    return output


def test_source_and_final_release_inspection_verify_pairing(
    assets: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    inspect_archive(assets / f"FPLinux-{RELEASE_ID}-sources.zip")
    assert "kind: corresponding-sources" in capsys.readouterr().out
    inspect_distribution(assets)
    assert "16 runtimes; corresponding sources and checksums: OK" in capsys.readouterr().out


def test_release_rejects_changed_runtime_even_after_outer_checksums_are_replaced(
    assets: Path,
) -> None:
    runtime = assets / f"FPLinux-nokia-ta1618-linux-x64-{RELEASE_ID}.zip"
    write_zip(runtime, {"README.txt": b"another runtime\n"}, set())
    (assets / "SHA256SUMS").write_text(
        "".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
            for path in sorted(assets.iterdir())
            if path.name != "SHA256SUMS"
        )
    )
    with pytest.raises(SystemExit, match="pairing hash differs"):
        inspect_distribution(assets)


def test_source_inspection_requires_the_selected_rebuild_material(assets: Path) -> None:
    source = assets / f"FPLinux-{RELEASE_ID}-sources.zip"
    files, modes = read_zip(source)
    files.pop("SHA256SUMS")
    files.pop("BUILD-LINUX.txt")
    write_zip(source, files, {name for name, mode in modes.items() if mode & 0o111})
    with pytest.raises(SystemExit, match=r"lack required build material: BUILD-LINUX\.txt"):
        inspect_archive(source)


def test_source_inspection_rejects_changed_project_bytes_with_consistent_zip_checksums(
    assets: Path,
) -> None:
    source = assets / f"FPLinux-{RELEASE_ID}-sources.zip"
    files, modes = read_zip(source)
    files.pop("SHA256SUMS")
    files["project/common/example.c"] = b"changed corresponding source\n"
    write_zip(source, files, {name for name, mode in modes.items() if mode & 0o111})
    with pytest.raises(SystemExit, match="sources differ from their release identity"):
        inspect_archive(source)


def test_release_inspection_rejects_changed_dependency_part(assets: Path) -> None:
    next(assets.glob("*.part-00000")).write_bytes(b"changed original\n")
    with pytest.raises(SystemExit, match="part is missing or mismatched"):
        inspect_distribution(assets)
