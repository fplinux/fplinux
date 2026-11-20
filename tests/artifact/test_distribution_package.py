# SPDX-License-Identifier: GPL-2.0-only
"""Exercise complete release assembly with independent ZIP and transport fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.ci.commands import run_ci
from fplinux_cli.cli.inspect import inspect_distribution
from fplinux_cli.packaging import distribution
from fplinux_cli.packaging.universal import HostArchive

from tests.distribution_support import (
    CARD_TARGETS,
    RELEASE_ID,
    SELECTIONS,
    bundle_fixture,
    json_bytes,
    read_zip,
    release_fields,
    write_release,
    write_transport,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def inputs(tmp_path: Path) -> tuple[Path, Path]:
    packages = tmp_path / "packages"
    write_release(packages)
    transport = tmp_path / "transport"
    write_transport(transport)
    return packages, transport


def test_release_contains_the_complete_matrix_sources_and_dependency_transport(
    inputs: tuple[Path, Path], tmp_path: Path
) -> None:
    packages, transport = inputs
    output = tmp_path / "final"

    distribution.attach_inputs(packages, transport, output, expected_matrix=SELECTIONS)
    source = output / f"FPLinux-{RELEASE_ID}-sources.zip"

    assert source.name == f"FPLinux-{RELEASE_ID}-sources.zip"
    expected = {
        f"FPLinux-linux-x64-{RELEASE_ID}.zip",
        f"FPLinux-windows-x64-{RELEASE_ID}.zip",
        source.name,
        "SHA256SUMS",
        "RELEASE.json",
        *(path.name for path in transport.iterdir()),
    }
    assert {path.name for path in output.iterdir()} == expected
    sources, modes = read_zip(source)
    assert modes["project/fplinux"] == 0o100755
    assert sources["project/common/example.c"] == b"one shared project source\n"
    assert "project/targets/konrow-senior-280-plus/target.toml" in sources
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
        catalog = json.loads(files["targets.json"])
        assert set(catalog["targets"]) == {t for t, h in SELECTIONS if h == record["host"]}
        target_files = catalog["targets"][record["target"]]["files"]
        assert ("card/layout.json" in target_files) == (record["target"] in CARD_TARGETS)
        if record["target"] in CARD_TARGETS:
            assert target_files["image/ramboot.bin"][1] in target_files["card/fit.itb"]
            assert (
                target_files["card/root.ext4.xz"]
                == (catalog["targets"]["inoi-240-modern-4g"]["files"]["card/root.ext4.xz"])
            )
        assert files["licenses/host-tools/COPYING"] == b"included host tool license\n"
        assert target_files["assets.lock.toml"] == [f"assets/{record['target']}.lock.toml"]
        ramboot = b"".join(files[part] for part in target_files["image/ramboot.bin"])
        original, _ = bundle_fixture(record["target"], record["host"], card=False)
        assert ramboot == original["image/ramboot.bin"]
        assert runtime.read_bytes() == (packages / record["archive"]).read_bytes()
        if record["target"] in CARD_TARGETS:
            sd = catalog["targets"][record["target"]]["microsd"]["files"]
            assert (
                b"".join(files[part] for part in sd["image/ramboot.bin"])
                == b"USB stage0 with U-Boot"
            )
            assert not any(name.startswith("card/") for name in sd)
        if record["target"] == "nokia-ta1618":
            other = catalog["targets"]["inoi-240-modern-4g"]["files"]["image/ramboot.bin"]
            assert target_files["image/ramboot.bin"][1:] == other[1:]
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


def test_ci_rejects_single_model_package_that_passes_intrinsic_verification(
    tmp_path: Path,
) -> None:
    packages = tmp_path / "packages"
    transport = tmp_path / "transport"
    output = tmp_path / "final"
    write_release(packages, selections=(("konrow-senior-280-plus", "linux"),))
    write_transport(transport)

    metadata = inspect_distribution(packages)
    assert metadata["dependencies"] is None
    assert [(record["target"], record["host"]) for record in metadata["runtimes"]] == [
        ("konrow-senior-280-plus", "linux"),
    ]
    with pytest.raises(SystemExit, match="complete supported target/host matrix"):
        run_ci(
            argparse.Namespace(
                ci_command="attach-inputs",
                directory=packages,
                inputs_dir=transport,
                output_directory=output,
            )
        )
    assert not output.exists()


def test_package_rejects_another_card_system(tmp_path: Path) -> None:
    linux = HostArchive("linux", RELEASE_ID)
    for target in ("nokia-ta1618", "inoi-240-modern-4g"):
        files, modes = bundle_fixture(target, "linux", card=True)
        if target == "inoi-240-modern-4g":
            files["card/root.ext4.xz"] = b"another system"
            with pytest.raises(SystemExit, match="different microSD base files"):
                linux.add(target, files, modes)
        else:
            linux.add(target, files, modes)
    assert not (tmp_path / "final").exists()


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


@pytest.mark.parametrize(
    "member", [".data/runtime/session.json", ".private/retained.txt", "device-data.cpio"]
)
def test_distribution_rejects_installation_data_and_private_work(member: str) -> None:
    with pytest.raises(SystemExit, match="prepared phone data"):
        distribution.require_public_runtime_files({"image/ramboot.bin", member}, "test ZIP")
