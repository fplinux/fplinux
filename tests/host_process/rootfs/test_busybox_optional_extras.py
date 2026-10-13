# SPDX-License-Identifier: GPL-2.0-only
"""Execute the BusyBox package recipe and native signed APK ownership transactions."""

from __future__ import annotations

import shutil
import stat
import subprocess
import tempfile
from pathlib import Path

import pytest

from tests import ROOT
from tests.apk_support import ApkIdentity, signed_apk_tree

RECIPE = ROOT / "alpine/aports/fplinux-busybox/APKBUILD"
INSTALLER = ROOT / "tests/fixtures/busybox/packaging-installer.sh"
FIXTURES = ROOT / "tests/fixtures/busybox"
CORE_PAYLOAD = (FIXTURES / "core-payload.sh").read_bytes()
EXTRAS_PAYLOAD = (FIXTURES / "extras-payload.sh").read_bytes()
EXTRAS_CONFIG = (FIXTURES / "extras-udhcpd.conf").read_bytes()


def _stage_core(temporary: Path, *, arch: str) -> tuple[Path, ApkIdentity]:
    """Execute recipe metadata and packaging with controlled external build outputs."""
    sources = temporary / "src"
    build = sources / "busybox-1.37.0"
    (build / "applets").mkdir(parents=True)
    shutil.copyfile(INSTALLER, build / "applets/install.sh")
    (build / ".config").write_text('CONFIG_PREFIX=""\n', encoding="utf-8")
    # The prebuilt programs are external fixtures, not BusyBox runtime proof.
    (build / "busybox").write_bytes(CORE_PAYLOAD)
    shutil.copyfile(FIXTURES / "busybox.links", build / "busybox.links")
    (build / "examples/udhcp").mkdir(parents=True)
    shutil.copyfile(FIXTURES / "core-udhcpd.conf", build / "examples/udhcp/udhcpd.conf")
    for name in (
        "udhcpc.conf",
        "securetty",
        "default.script",
        "dad.if-up",
        "utmps-COPYING",
        "skalibs-COPYING",
    ):
        (sources / name).write_text(f"controlled {name} input\n", encoding="utf-8")
    stage = temporary / "core-stage"
    metadata = temporary / "core-metadata"
    staged = subprocess.run(
        [
            "sh",
            "-eu",
            str(FIXTURES / "package-core.sh"),
            str(RECIPE),
            str(sources),
            str(stage),
            str(metadata),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    if staged.returncode != 0:
        raise AssertionError(staged.stdout + staged.stderr)
    name, version, provides, depends, replaces = metadata.read_text(encoding="utf-8").splitlines()
    identity = ApkIdentity(
        name=name,
        arch=arch,
        version=version,
        provides=tuple(provides.split()),
        depends=tuple(depends.split()),
        replaces=tuple(replaces.split()),
    )
    return stage, identity


@pytest.mark.skipif(
    not (shutil.which("apk") and shutil.which("openssl")),
    reason="native apk and openssl are required for signed APK process tests",
)
class BusyBoxOptionalExtrasTests:
    """Protect optional configuration ownership; no BusyBox applets or phone are tested."""

    def test_extras_installs_default_configuration_without_replacing_core(self) -> None:
        """Real APK add/remove transactions preserve the recipe's core payload."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            arch = subprocess.run(
                ["apk", "--print-arch"],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            ).stdout.strip()
            core, core_identity = _stage_core(temporary, arch=arch)
            assert ((core / "bin/busybox").read_bytes()) == (CORE_PAYLOAD)
            assert (stat.S_IMODE((core / "bin/busybox").stat().st_mode)) == (0o755)
            extra = temporary / "extras-stage"
            (extra / "etc").mkdir(parents=True)
            (extra / "etc/udhcpd.conf").write_bytes(EXTRAS_CONFIG)
            (extra / "bin").mkdir()
            (extra / "bin/busybox-extras").write_bytes(EXTRAS_PAYLOAD)
            (extra / "bin/busybox-extras").chmod(0o755)
            keys = temporary / "keys"
            keys.mkdir()
            private_key = temporary / "test.rsa"
            subprocess.run(
                ["openssl", "genrsa", "-out", str(private_key), "2048"],
                capture_output=True,
                check=True,
                timeout=10,
            )
            subprocess.run(
                [
                    "openssl",
                    "rsa",
                    "-in",
                    str(private_key),
                    "-pubout",
                    "-out",
                    str(keys / "sysroot-test.rsa.pub"),
                ],
                capture_output=True,
                check=True,
                timeout=10,
            )
            core_apk = signed_apk_tree(
                temporary,
                core_identity,
                core,
                private_key=private_key,
            )
            extra_apk = signed_apk_tree(
                temporary,
                ApkIdentity("busybox-extras", arch, "1.37.0-r31", depends=("busybox=1.37.0-r31",)),
                extra,
                private_key=private_key,
            )
            sysroot = temporary / "root"
            (sysroot / "proc/self").mkdir(parents=True)
            (sysroot / "dev").mkdir()
            (sysroot / "dev/null").touch()
            (sysroot / "etc/apk/cache").mkdir(parents=True)
            command = [
                "apk",
                "--root",
                str(sysroot),
                "--arch",
                arch,
                "--no-network",
                "--no-scripts",
                "--no-logfile",
                "--keys-dir",
                str(keys),
                "--repositories-file",
                "/dev/null",
            ]
            installed = subprocess.run(
                [*command, "--initdb", "add", str(core_apk)],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            assert (installed.returncode) == (0), installed.stdout + installed.stderr
            simulated = subprocess.run(
                [*command, "add", "--simulate", str(extra_apk)],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            assert (simulated.returncode) == (0), simulated.stdout + simulated.stderr
            added = subprocess.run(
                [*command, "add", str(extra_apk)],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            assert (added.returncode) == (0), added.stdout + added.stderr
            assert ((sysroot / "etc/udhcpd.conf").read_bytes()) == (EXTRAS_CONFIG)
            assert ((sysroot / "bin/busybox-extras").read_bytes()) == (EXTRAS_PAYLOAD)
            assert ((sysroot / "bin/busybox").read_bytes()) == (CORE_PAYLOAD)
            assert (stat.S_IMODE((sysroot / "bin/busybox").stat().st_mode)) == (0o755)
            removed = subprocess.run(
                [*command, "del", "busybox-extras"],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            assert (removed.returncode) == (0), removed.stdout + removed.stderr
            assert not ((sysroot / "etc/udhcpd.conf").exists())
            assert ((sysroot / "bin/busybox").read_bytes()) == (CORE_PAYLOAD)
            assert (stat.S_IMODE((sysroot / "bin/busybox").stat().st_mode)) == (0o755)
