# SPDX-License-Identifier: GPL-2.0-only
"""Execute the BusyBox package recipe and native signed APK ownership transactions."""

from __future__ import annotations

import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from tests.apk_support import ApkIdentity, signed_apk_tree

ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / "alpine/aports/fplinux-busybox/APKBUILD"
INSTALLER = ROOT / "tests/fixtures/busybox/packaging-installer.sh"
CORE_PAYLOAD = b"#!/bin/sh\nprintf 'core payload fixture\\n'\n"
EXTRAS_PAYLOAD = b"#!/bin/sh\nprintf 'extras help fixture\\n'\n"
EXTRAS_CONFIG = b"start 192.0.2.20\nend 192.0.2.30\ninterface test0\n"


def _stage_core(temporary: Path) -> Path:
    """Run the production package function with controlled external build outputs."""
    sources = temporary / "src"
    build = sources / "busybox-1.37.0"
    (build / "applets").mkdir(parents=True)
    shutil.copyfile(INSTALLER, build / "applets/install.sh")
    (build / ".config").write_text('CONFIG_PREFIX=""\n', encoding="utf-8")
    # The prebuilt programs are external fixtures, not BusyBox runtime proof.
    (build / "busybox").write_bytes(CORE_PAYLOAD)
    (build / "busybox.links").write_text(
        "/bin/sh\n/sbin/ifup\n/sbin/ifdown\n/usr/bin/wget\n/usr/sbin/udhcpd\n",
        encoding="utf-8",
    )
    (build / "examples/udhcp").mkdir(parents=True)
    (build / "examples/udhcp/udhcpd.conf").write_bytes(
        b"start 192.0.2.2\nend 192.0.2.10\ninterface core0\n"
    )
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
    staged = subprocess.run(
        [
            "sh",
            "-eu",
            "-c",
            'srcdir=$2; pkgdir=$3; . "$1"; cd "$builddir"; package',
            "busybox-package",
            str(RECIPE),
            str(sources),
            str(stage),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )
    if staged.returncode != 0:
        raise AssertionError(staged.stdout + staged.stderr)
    return stage


@unittest.skipUnless(
    shutil.which("apk") and shutil.which("openssl"),
    "native apk and openssl are required for signed APK process tests",
)
class BusyBoxOptionalExtrasTests(unittest.TestCase):
    """Protect optional configuration ownership; no BusyBox applets or phone are tested."""

    def test_extras_installs_default_configuration_without_replacing_core(self) -> None:
        """Real APK add/remove transactions preserve the recipe's core payload."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            core = _stage_core(temporary)
            self.assertEqual((core / "bin/busybox").read_bytes(), CORE_PAYLOAD)
            self.assertEqual(stat.S_IMODE((core / "bin/busybox").stat().st_mode), 0o755)
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
            arch = subprocess.run(
                ["apk", "--print-arch"],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            ).stdout.strip()
            core_apk = signed_apk_tree(
                temporary,
                ApkIdentity("fplinux-busybox", arch, "1.37.0-r0", ("busybox=1.37.0-r31",)),
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
            self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
            simulated = subprocess.run(
                [*command, "add", "--simulate", str(extra_apk)],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(simulated.returncode, 0, simulated.stdout + simulated.stderr)
            added = subprocess.run(
                [*command, "add", str(extra_apk)],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(added.returncode, 0, added.stdout + added.stderr)
            self.assertEqual((sysroot / "etc/udhcpd.conf").read_bytes(), EXTRAS_CONFIG)
            self.assertEqual((sysroot / "bin/busybox-extras").read_bytes(), EXTRAS_PAYLOAD)
            self.assertEqual((sysroot / "bin/busybox").read_bytes(), CORE_PAYLOAD)
            self.assertEqual(stat.S_IMODE((sysroot / "bin/busybox").stat().st_mode), 0o755)
            removed = subprocess.run(
                [*command, "del", "busybox-extras"],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
            self.assertFalse((sysroot / "etc/udhcpd.conf").exists())
            self.assertEqual((sysroot / "bin/busybox").read_bytes(), CORE_PAYLOAD)
            self.assertEqual(stat.S_IMODE((sysroot / "bin/busybox").stat().st_mode), 0o755)


if __name__ == "__main__":
    unittest.main()
