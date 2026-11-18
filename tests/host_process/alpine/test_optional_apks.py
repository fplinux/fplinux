# SPDX-License-Identifier: GPL-2.0-only
"""Check flat optional APK delivery against a native installed package database."""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.rootfs_verify import _require_bundle_packages_installable

from tests.apk_support import ApkIdentity, signed_apk_tree

if TYPE_CHECKING:
    from pathlib import Path

pytestmark = pytest.mark.skipif(
    not all(shutil.which(command) for command in ("apk", "openssl")),
    reason="native apk and openssl are required for optional APK process tests",
)


@dataclass(frozen=True)
class OptionalApks:
    """Own an installed base and optional packages with one additional runtime dependency."""

    root: Path
    application: Path
    runtime: Path


@pytest.fixture
def optional_apks(tmp_path: Path) -> OptionalApks:
    """Build signed test packages independently and install only the exact base."""
    root = tmp_path / "root"
    (root / "proc/self").mkdir(parents=True)
    (root / "dev").mkdir()
    (root / "dev/null").touch()
    (root / "etc/apk/cache").mkdir(parents=True)
    keys = root / "etc/apk/keys"
    keys.mkdir()
    private_key = tmp_path / "test.rsa"
    public_key = keys / "sysroot-test.rsa.pub"
    subprocess.run(
        ["openssl", "genrsa", "-out", str(private_key), "2048"],
        capture_output=True,
        check=True,
        timeout=10,
    )
    subprocess.run(
        ["openssl", "rsa", "-in", str(private_key), "-pubout", "-out", str(public_key)],
        capture_output=True,
        check=True,
        timeout=10,
    )
    architecture = subprocess.run(
        ["apk", "--print-arch"],
        capture_output=True,
        text=True,
        check=True,
        timeout=10,
    ).stdout.strip()
    packages = {}
    for name, dependencies in (
        ("fixture-base", ()),
        ("fixture-runtime", ("fixture-base",)),
        ("fixture-application", ("fixture-runtime",)),
    ):
        tree = tmp_path / "payload" / name
        (tree / "usr/share/fixture").mkdir(parents=True)
        (tree / "usr/share/fixture" / name).write_bytes(name.encode())
        packages[name] = signed_apk_tree(
            tmp_path,
            ApkIdentity(name=name, arch=architecture, depends=dependencies),
            tree,
            private_key=private_key,
        )
    installed = subprocess.run(
        [
            "apk",
            "--root",
            str(root),
            "--arch",
            architecture,
            "--initdb",
            "--no-network",
            "--no-scripts",
            "--keys-dir",
            str(keys),
            "--repositories-file",
            "/dev/null",
            "add",
            str(packages["fixture-base"]),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    assert installed.returncode == 0, installed.stdout + installed.stderr
    return OptionalApks(
        root=root,
        application=packages["fixture-application"],
        runtime=packages["fixture-runtime"],
    )


@pytest.mark.parametrize("complete", [False, True], ids=["missing-runtime", "complete-set"])
def test_optional_apks_require_their_additional_runtime_offline(
    optional_apks: OptionalApks, *, complete: bool
) -> None:
    """The native solver accepts the full closure and rejects an unresolved application."""
    packages = (
        (optional_apks.application, optional_apks.runtime)
        if complete
        else (optional_apks.application,)
    )
    database = optional_apks.root / "lib/apk/db/installed"
    world = optional_apks.root / "etc/apk/world"
    database_before = database.read_bytes()
    world_before = world.read_bytes()

    if complete:
        _require_bundle_packages_installable(optional_apks.root, packages)
    else:
        with pytest.raises(
            SystemExit, match="cannot be installed into the rootfs offline"
        ) as error:
            _require_bundle_packages_installable(optional_apks.root, packages)
        assert "fixture-runtime" in str(error.value)

    assert database.read_bytes() == database_before
    assert world.read_bytes() == world_before
    assert not (optional_apks.root / "usr/share/fixture/fixture-runtime").exists()
    assert not (optional_apks.root / "usr/share/fixture/fixture-application").exists()
