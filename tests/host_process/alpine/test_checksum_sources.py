# SPDX-License-Identifier: GPL-2.0-only
"""Resolved originals and local checksums reach native Alpine package tools."""

from __future__ import annotations

import hashlib
import io
import os
import shutil
import tarfile
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.aports import materialize_aport_sources
from fplinux_cli.alpine.sources import replace_checksum_block
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.snapshots import preserve_inputs, restore_manifest_inputs

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

_FIXTURES = ROOT / "tests/host_process/alpine/fixtures/source-materialization"


@dataclass(frozen=True)
class MaterializationInputs:
    """A canonical recipe, its retained originals and one writable native stage."""

    root: Path
    cache: Path
    stage: Path
    originals: tuple[Path, ...]

    def materialize(self, *, verify_local: bool = True) -> None:
        """Stage the recipe's resolved originals through the package source boundary."""
        materialize_aport_sources(
            "fplinux-materialization-fixture",
            self.root,
            self.stage,
            cache=self.cache,
            verify_local=verify_local,
        )


def _archive(path: Path, member: str, contents: bytes) -> None:
    with tarfile.open(path, "w:gz") as archive:
        entry = tarfile.TarInfo(member)
        entry.size = len(contents)
        entry.mode = 0o644
        archive.addfile(entry, io.BytesIO(contents))


@pytest.fixture
def materialization_inputs(tmp_path: Path) -> MaterializationInputs:
    """Restore private originals by native checksums into their canonical locations."""
    root = tmp_path / "checkout"
    aport = root / "alpine/aports/fplinux-materialization-fixture"
    aport.mkdir(parents=True)
    apkbuild = aport / "APKBUILD"
    shutil.copyfile(_FIXTURES / "APKBUILD.fixture", apkbuild)
    shutil.copyfile(_FIXTURES / "local.patch", aport / "local.patch")
    local_checksum = hashlib.sha512((aport / "local.patch").read_bytes()).hexdigest()
    apkbuild.write_bytes(
        replace_checksum_block(
            apkbuild.read_bytes(),
            {"local.patch": local_checksum},
            path=apkbuild,
        )
    )
    retained = tmp_path / "retained"
    retained.mkdir()
    main = retained / "sample.tar.gz"
    extra = retained / "extra.tar.gz"
    upstream = retained / "upstream.patch"
    _archive(main, "sample-1.2/value.txt", b"initial\n")
    _archive(extra, "extra-4.5/extra.txt", b"secondary source\n")
    shutil.copyfile(_FIXTURES / "upstream.patch", upstream)
    records = []
    catalog = []
    for name, version, source in (
        ("main", "1.2", main),
        ("extra", "4.5", extra),
        ("upstream_patch", "1", upstream),
    ):
        checksum = hashlib.sha512(source.read_bytes()).hexdigest()
        source.chmod(0o600)
        destination = f"downloads/sources/{name}/{source.name}"
        records.append(
            DependencyInput(
                f"source:{name}",
                f"https://example.invalid/{source.name}",
                None,
                source.stat().st_size,
                destination,
                "source-original",
                checksum=checksum,
                algorithm="sha512",
            )
        )
        catalog.append(
            f'[{name}]\nversion="{version}"\nname="{source.name}"\n'
            f'url="https://example.invalid/{source.name}"\nalgorithm="sha512"\n'
            f'checksum="{checksum}"\nbytes={source.stat().st_size}\n'
        )
    (root / "sources.lock.toml").write_text("\n".join(catalog))
    cache = tmp_path / "cache"
    snapshot = tmp_path / "snapshot"
    manifest = preserve_inputs(
        records, {}, snapshot, cache=cache, offline=True, sources=[retained]
    )
    restore_manifest_inputs(snapshot, manifest, cache)
    originals = tuple(cache / item.destination for item in records)
    return MaterializationInputs(root, cache, tmp_path / "stage", originals)


def test_native_unpack_and_patch_order_use_verified_local_originals(
    materialization_inputs: MaterializationInputs,
) -> None:
    """The staged recipe passes native lint and applies upstream then local patches."""
    context = materialization_inputs
    context.materialize()
    run_process(
        ["apkbuild-lint", "APKBUILD"],
        name="lint materialized APKBUILD",
        timeout=30,
        cwd=context.stage,
        check=True,
    )
    environment = os.environ | {"SRCDEST": str(context.cache / "native-sources")}
    run_process(
        ["abuild", "-F", "fetch", "verify", "unpack", "prepare"],
        name="unpack verified native sources and apply ordered patches",
        timeout=30,
        cwd=context.stage,
        env=environment,
        check=True,
    )
    assert (context.stage / "src/sample-1.2/value.txt").read_bytes() == b"final\n"
    assert (context.stage / "src/extra-4.5/extra.txt").read_bytes() == b"secondary source\n"
    assert all(path.stat().st_mode & 0o777 == 0o600 for path in context.originals)
    assert all(path.stat().st_uid == os.getuid() for path in context.originals)


def test_upstream_pin_mismatch_stops_before_native_checksum_generation(
    materialization_inputs: MaterializationInputs,
) -> None:
    context = materialization_inputs
    context.originals[0].write_bytes(b"untrusted upstream bytes")
    before = (context.root / "alpine/aports/fplinux-materialization-fixture/APKBUILD").read_bytes()
    with pytest.raises(SystemExit, match="offline locked input is missing or mismatched"):
        context.materialize(verify_local=False)
    assert (
        context.root / "alpine/aports/fplinux-materialization-fixture/APKBUILD"
    ).read_bytes() == before


def test_local_patch_mismatch_is_rejected_without_regenerating_its_checksum(
    materialization_inputs: MaterializationInputs,
) -> None:
    context = materialization_inputs
    patch = context.root / "alpine/aports/fplinux-materialization-fixture/local.patch"
    patch.write_bytes(b"changed local patch\n")
    before = (patch.parent / "APKBUILD").read_bytes()
    with pytest.raises(SystemExit, match="local Alpine source SHA-512 mismatch"):
        context.materialize()
    assert (patch.parent / "APKBUILD").read_bytes() == before


@pytest.mark.parametrize("verify_local", [True, False], ids=["normal-build", "checksum"])
def test_new_local_source_requires_native_checksum_regeneration(
    materialization_inputs: MaterializationInputs,
    *,
    verify_local: bool,
) -> None:
    """Only checksum preparation admits a new local file without an existing checksum."""
    context = materialization_inputs
    aport = context.root / "alpine/aports/fplinux-materialization-fixture"
    data = b"int new_local_source;\n"
    (aport / "new-local.c").write_bytes(data)
    apkbuild = aport / "APKBUILD"
    apkbuild.write_text(
        apkbuild.read_text().replace("\tlocal.patch\n", "\tlocal.patch\n\tnew-local.c\n")
    )
    if verify_local:
        with pytest.raises(SystemExit, match="local Alpine source checksum inventory differs"):
            context.materialize()
        return

    context.materialize(verify_local=False)
    run_process(
        ["abuild", "-F", "checksum", "verify"],
        name="generate and verify a new local source checksum with native abuild",
        timeout=30,
        cwd=context.stage,
        env=os.environ | {"SRCDEST": str(context.cache / "native-sources")},
        check=True,
    )
    expected = f"{hashlib.sha512(data).hexdigest()}  new-local.c\n".encode()
    assert expected in (context.stage / "APKBUILD").read_bytes()
