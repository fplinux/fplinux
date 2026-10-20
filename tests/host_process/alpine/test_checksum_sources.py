# SPDX-License-Identifier: GPL-2.0-only
"""Checksum generation reads restored private sources through real Alpine tools."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path

import pytest
from fplinux_cli.alpine.checksum import _checksum_container_command
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.snapshots import preserve_inputs, restore_manifest_inputs

from tests import ROOT

_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
_ABC_SHA512 = (
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2"
    "192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
)


def _checksum_invocation(stage: Path, downloads: Path) -> list[str]:
    """Replace only the isolated mount mapping, keeping real Alpine tools and users."""
    command = _checksum_container_command(
        "kern",
        package="fplinux-checksum-fixture",
        rootfs=Path("/checksum-environment"),
        offline=True,
        stage=stage,
        downloads=downloads,
    )
    return [
        argument.replace("/cache/downloads", str(downloads)).replace("/workspace", str(stage))
        for argument in command[command.index("--") + 1 :]
    ]


def _fetch_guard(directory: Path, marker: Path) -> dict[str, str]:
    """Record fetch requests while refusing any uncached external source."""
    tools = directory / "tools"
    tools.mkdir()
    guard = tools / "abuild-fetch"
    shutil.copyfile(
        ROOT / "tests/host_process/alpine/fixtures/checksum-sources/abuild-fetch.sh", guard
    )
    guard.chmod(0o755)
    return os.environ | {
        "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "FPLINUX_CHECKSUM_FETCH_MARKER": str(marker),
    }


@pytest.mark.parametrize("empty_checksums", [False, True], ids=["outdated", "empty"])
def test_checksum_reads_private_restored_source_and_returns_cache_ownership(
    *, empty_checksums: bool
) -> None:
    """A restored 0600 source remains reusable after real checksum generation."""
    assert os.getuid() == 0, "checksum process test requires the pinned root environment"
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        directory.chmod(0o755)
        original = directory / "original.tar.gz"
        original.write_bytes(b"abc")
        original.chmod(0o600)
        cache = directory / "cache"
        downloads = cache / "downloads"
        downloads.mkdir(parents=True)
        snapshot = directory / "snapshot"
        manifest = preserve_inputs(
            [
                DependencyInput(
                    key="checksum-fixture",
                    url="https://example.invalid/fixture-1.tar.gz",
                    sha256=_ABC_SHA256,
                    size=3,
                    destination="downloads/alpine/sources/fixture-1.tar.gz",
                    purpose="checksum process fixture",
                )
            ],
            {},
            snapshot,
            cache=cache,
            offline=True,
            sources=[original],
        )
        restore_manifest_inputs(snapshot, manifest, cache)
        source = downloads / "alpine/sources/fixture-1.tar.gz"
        source_before = source.stat()
        assert (source_before.st_uid, source_before.st_gid) == (0, 0)
        assert stat.S_IMODE(source_before.st_mode) == 0o600

        stage = directory / "stage"
        aport = stage / "alpine/aports/fplinux-checksum-fixture"
        aport.mkdir(parents=True)
        apkbuild = aport / "APKBUILD"
        shutil.copyfile(ROOT / "tests/fixtures/checksum-sources/APKBUILD", apkbuild)
        if empty_checksums:
            recipe = apkbuild.read_bytes().partition(b'sha512sums="')[0]
            apkbuild.write_bytes(recipe + b'sha512sums=""\n')
        shutil.copytree(ROOT / "scripts/fplinux_cli/alpine/checksum", stage / "checksum")
        marker = aport / "fetch-invocations"
        result = subprocess.run(
            _checksum_invocation(stage, downloads),
            cwd=aport,
            env=_fetch_guard(directory, marker),
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

        assert result.returncode == 0, result.stdout + result.stderr
        assert marker.read_text() == "fixture-1.tar.gz::https://example.invalid/fixture-1.tar.gz\n"
        metadata = subprocess.run(
            ["sh", "-ceu", 'srcdir=$PWD; . ./APKBUILD; printf "%s\\n" "$sha512sums"'],
            cwd=aport,
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        )
        assert metadata.stdout.split() == [_ABC_SHA512, "fixture-1.tar.gz"]
        assert source.read_bytes() == b"abc"
        source_after = source.stat()
        assert (source_after.st_uid, source_after.st_gid) == (0, 0)
        assert stat.S_IMODE(source_after.st_mode) == 0o600
        assert (apkbuild.stat().st_uid, apkbuild.stat().st_gid) == (0, 0)


def test_checksum_missing_offline_source_fails_before_fetch_or_recipe_edit() -> None:
    """Report the required archive without trying a downloader or changing the recipe."""
    assert os.getuid() == 0, "checksum process test requires the pinned root environment"
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        directory.chmod(0o755)
        downloads = directory / "downloads"
        downloads.mkdir()
        stage = directory / "stage"
        aport = stage / "alpine/aports/fplinux-checksum-fixture"
        aport.mkdir(parents=True)
        apkbuild = aport / "APKBUILD"
        shutil.copyfile(ROOT / "tests/fixtures/checksum-sources/APKBUILD", apkbuild)
        before = apkbuild.read_bytes()
        shutil.copytree(ROOT / "scripts/fplinux_cli/alpine/checksum", stage / "checksum")
        marker = aport / "fetch-invocations"

        result = subprocess.run(
            _checksum_invocation(stage, downloads),
            cwd=aport,
            env=_fetch_guard(directory, marker),
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

        assert result.returncode != 0
        assert "offline checksum requires cached source: fixture-1.tar.gz" in result.stderr
        assert (
            "./fplinux checksum fplinux-checksum-fixture without --offline first" in result.stderr
        )
        assert not marker.exists(), result.stdout + result.stderr
        assert apkbuild.read_bytes() == before
        assert not (downloads / "alpine/sources/fixture-1.tar.gz").exists()
