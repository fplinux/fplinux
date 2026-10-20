# SPDX-License-Identifier: GPL-2.0-only
"""Managed-cache removal across a real subordinate UID/GID namespace."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.cache.prune import alpine as prune_alpine
from fplinux_cli.cache.prune.operations import apply_prune, discard_obsolete_rootfs

from tests import ROOT
from tests.fixtures.executables import install_python_script

if TYPE_CHECKING:
    from collections.abc import Iterator

PRUNE_FIXTURES = ROOT / "tests/host_process/cache/fixtures/prune-rootfs"


@pytest.fixture
def namespace() -> list[str]:
    """Require the host namespace used by container ownership and managed removal."""
    unshare = shutil.which("unshare")
    if unshare is None:
        pytest.skip("subordinate namespace test requires unshare")
    command = [unshare, "--map-auto", "--map-root-user", "--"]
    probe = subprocess.run(
        [*command, "true"], capture_output=True, text=True, check=False, timeout=30
    )
    if probe.returncode:
        pytest.skip("subordinate namespace is unavailable: " + probe.stderr.strip())
    return command


@pytest.fixture
def cache_root(namespace: list[str]) -> Iterator[Path]:
    """Own an isolated cache and recover mapped-user files after failed assertions."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        try:
            yield root
        finally:
            subprocess.run(
                [*namespace, "rm", "--recursive", "--force", "--", str(root)],
                check=True,
                timeout=30,
            )


def _prepare(root: Path, namespace: list[str]) -> tuple[Path, Path, Path]:
    """Build the real mapped-owner boundary while keeping recipe selection local."""
    cache = root / ".cache"
    candidate = cache / "rootfs/.abandoned-build"
    candidate.mkdir(parents=True)
    external = root / "external"
    external.mkdir()
    sentinel = external / "sentinel"
    sentinel.write_bytes(b"keep external data\n")
    prepared = subprocess.run(
        [
            *namespace,
            sys.executable,
            str(PRUNE_FIXTURES / "prepare_mapped_rootfs.py"),
            str(candidate),
            str(external),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert prepared.returncode == 0, prepared.stderr
    assert (candidate / "package-work/home").stat().st_uid != os.geteuid()
    return cache, candidate, sentinel


class PruneRootfsOwnershipTests:
    """Recipe discovery is controlled; UID mappings, files and deletion are real."""

    @pytest.mark.parametrize("cleanup", ["prune", "build-retention"])
    def test_removes_abandoned_builder_owned_rootfs(
        self, cache_root: Path, namespace: list[str], cleanup: str
    ) -> None:
        """Both managed removal consumers discard private build-user directories."""
        cache, candidate, sentinel = _prepare(cache_root, namespace)
        current = cache / "rootfs/current"
        current.mkdir()
        current_state = current / "state"
        current_state.write_bytes(b"retain current rootfs\n")
        with mock.patch.object(prune_alpine, "_current_rootfs_recipes", return_value={"current"}):
            if cleanup == "prune":
                removed = apply_prune(cache).removed
            else:
                removed = discard_obsolete_rootfs(cache)

        assert removed == ("rootfs/.abandoned-build",)
        assert not candidate.exists()
        assert sentinel.read_bytes() == b"keep external data\n"
        assert current_state.read_bytes() == b"retain current rootfs\n"

    def test_namespace_failure_does_not_report_removed(
        self, cache_root: Path, namespace: list[str], monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A failed namespace process propagates instead of acknowledging removal."""
        cache, candidate, sentinel = _prepare(cache_root, namespace)
        binary_directory = cache_root / "bin"
        binary_directory.mkdir()
        install_python_script(
            PRUNE_FIXTURES / "namespace_failure.py", binary_directory / "unshare"
        )
        monkeypatch.setenv("PATH", str(binary_directory) + os.pathsep + os.defpath)
        with (
            mock.patch.object(prune_alpine, "_current_rootfs_recipes", return_value=frozenset()),
            pytest.raises(subprocess.CalledProcessError) as failure,
        ):
            discard_obsolete_rootfs(cache)

        assert failure.value.returncode == 7
        assert candidate.is_dir()
        assert sentinel.read_bytes() == b"keep external data\n"
