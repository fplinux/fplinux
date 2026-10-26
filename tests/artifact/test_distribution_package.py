# SPDX-License-Identifier: GPL-2.0-only
"""Exercise complete release assembly with independent ZIP and transport fixtures."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cli.inspect import inspect_distribution
from fplinux_cli.packaging import distribution

from tests.distribution_support import (
    RELEASE_ID,
    SELECTIONS,
    json_bytes,
    read_zip,
    release_fields,
    write_pair,
    write_transport,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def inputs(tmp_path: Path) -> tuple[Path, Path]:
    packages = tmp_path / "packages"
    packages.mkdir()
    for target, host in SELECTIONS:
        write_pair(packages, target, host)
    transport = tmp_path / "transport"
    write_transport(transport)
    return packages, transport


def test_release_contains_the_complete_matrix_sources_and_dependency_transport(
    inputs: tuple[Path, Path], tmp_path: Path
) -> None:
    packages, transport = inputs
    output = tmp_path / "final"

    source = distribution.assemble_distribution(packages, output, inputs_directory=transport)

    assert source.name == f"FPLinux-{RELEASE_ID}-sources.zip"
    expected = {
        *(f"FPLinux-{target}-{host}-x64-{RELEASE_ID}.zip" for target, host in SELECTIONS),
        source.name,
        "SHA256SUMS",
        "RELEASE.json",
        *(path.name for path in transport.iterdir()),
    }
    assert {path.name for path in output.iterdir()} == expected
    sources, modes = read_zip(source)
    assert modes["project/fplinux"] == 0o100755
    assert sources["project/common/example.c"] == b"one shared project source\n"
    assert sources["upstream/shared.tar.xz"] == b"same exact upstream input"
    assert sources["licenses/host-tools/COPYING"] == b"retained license\n"
    metadata = json.loads((output / "RELEASE.json").read_bytes())
    assert {key: metadata[key] for key in release_fields()} == release_fields()
    assert metadata["sources"] == {
        "archive": source.name,
        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    }
    assert metadata["dependencies"] == "DEPENDENCIES.json"
    records = json.loads(sources["DISTRIBUTION.json"])["runtimes"]
    assert metadata["runtimes"] == records
    assert {(record["target"], record["host"]) for record in records} == set(SELECTIONS)
    for record in records:
        runtime = output / record["archive"]
        assert hashlib.sha256(runtime.read_bytes()).hexdigest() == record["sha256"]
        files, _modes = read_zip(runtime)
        assert f"  {source.name}\n".encode() in files["README.txt"]
        assert (
            not {"device-data.cpio", "CANDIDATE-NOTICE.txt", "DEVELOPMENT-NOTICE.txt"}
            & files.keys()
        )
        manifest = json.loads(files["build-manifest.json"])
        assert (
            manifest["files"]["README.txt"]["sha256"]
            == hashlib.sha256(files["README.txt"]).hexdigest()
        )
        selection = f"selections/{record['target']}/{record['host']}"
        assert (
            sources[f"{selection}/sources/host-tools.zip"]
            == f"source projection for {record['target']}/{record['host']}\n".encode()
        )
    checksums = dict(
        line.split("  ")[::-1] for line in (output / "SHA256SUMS").read_text().splitlines()
    )
    assert set(checksums) == expected - {"SHA256SUMS"}
    for name, digest in checksums.items():
        assert hashlib.sha256((output / name).read_bytes()).hexdigest() == digest
    inspect_distribution(output)


@pytest.mark.parametrize(
    ("damage", "diagnostic"),
    [
        ("missing", "matrix is incomplete"),
        ("conflicting_source", "conflicting corresponding source"),
        ("private_runtime", "prepared phone data"),
        ("mismatched_pair", "sources do not match runtime"),
        ("transport", "part is missing or mismatched"),
    ],
)
def test_release_rejects_incomplete_conflicting_private_or_unpaired_bytes(
    inputs: tuple[Path, Path], tmp_path: Path, damage: str, diagnostic: str
) -> None:
    packages, transport = inputs
    if damage == "missing":
        (packages / f"FPLinux-nokia-ta1618-windows-x64-{RELEASE_ID}-sources.zip").unlink()
    elif damage == "transport":
        next(transport.glob("*.part-00000")).write_bytes(b"changed original")
    else:
        write_pair(packages, "nokia-ta1618", "linux", **{damage: True})
    with pytest.raises(SystemExit, match=diagnostic):
        distribution.assemble_distribution(
            packages, tmp_path / "final", inputs_directory=transport
        )
    assert not (tmp_path / "final").exists()
    assert not list(tmp_path.glob(".fplinux-distribution-*"))


def test_source_distribution_parser_rejects_duplicate_runtime_selection() -> None:
    record = {
        "archive": f"FPLinux-nokia-ta1618-linux-x64-{RELEASE_ID}.zip",
        "target": "nokia-ta1618",
        "host": "linux",
        "identity": "f" * 64,
        "sha256": "a" * 64,
    }
    with pytest.raises(SystemExit, match="repeats a runtime"):
        distribution.distribution_records(
            json_bytes({**release_fields(), "runtimes": [record, record]})
        )
