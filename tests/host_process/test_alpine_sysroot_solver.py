# SPDX-License-Identifier: GPL-2.0-only
"""Exercise sysroot package replacement with native apk and isolated signed APKs."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.alpine_builder import alpine_sysroot_command

from tests.apk_support import signed_apk


@unittest.skipUnless(
    shutil.which("apk") and shutil.which("openssl"),
    "native apk and openssl are required for signed APK process tests",
)
class AlpineSysrootSolverTests(unittest.TestCase):
    """Observe native solver transactions; this does not test a build or a phone."""

    def test_replacement_releases_old_file_pin_and_preserves_unrelated_pin(self) -> None:
        """A local versioned provider replaces a pinned package without losing other intent."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            sysroot = temporary / "sysroot"
            # No scripts use these paths. Presence markers avoid mount probes,
            # and a native APK cache permits local packages on a temporary root.
            (sysroot / "proc/self").mkdir(parents=True)
            (sysroot / "dev").mkdir()
            (sysroot / "dev/null").touch()
            (sysroot / "etc/apk/cache").mkdir(parents=True)
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
            old = signed_apk(
                temporary,
                "stock-library",
                {"usr/include/library.h": b"stock header\n", "usr/share/stock.txt": b"stock\n"},
                arch=arch,
                private_key=private_key,
            )
            unrelated = signed_apk(
                temporary,
                "unrelated-library",
                {"usr/share/unrelated.txt": b"unrelated payload\n"},
                arch=arch,
                private_key=private_key,
            )
            replacement = signed_apk(
                temporary,
                "project-library",
                {"usr/include/library.h": b"project header\n"},
                arch=arch,
                private_key=private_key,
                replacement="stock-library",
            )
            lock = {"arch": arch}
            prepared = subprocess.run(
                alpine_sysroot_command(lock, [old, unrelated], sysroot, keys),
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)
            world_before = (sysroot / "etc/apk/world").read_text().splitlines()
            old_pin = next(entry for entry in world_before if entry.startswith("stock-library><"))
            unrelated_pin = next(
                entry for entry in world_before if entry.startswith("unrelated-library><")
            )
            self.assertEqual((sysroot / "usr/include/library.h").read_bytes(), b"stock header\n")

            replaced = subprocess.run(
                alpine_sysroot_command(lock, [replacement], sysroot, keys),
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(replaced.returncode, 0, replaced.stdout + replaced.stderr)
            self.assertEqual((sysroot / "usr/include/library.h").read_bytes(), b"project header\n")
            self.assertFalse((sysroot / "usr/share/stock.txt").exists())
            self.assertEqual(
                (sysroot / "usr/share/unrelated.txt").read_bytes(), b"unrelated payload\n"
            )
            world_after = (sysroot / "etc/apk/world").read_text().splitlines()
            self.assertIn(unrelated_pin, world_after)
            self.assertNotIn(old_pin, world_after)
            self.assertTrue(any(entry.startswith("project-library><") for entry in world_after))
            installed = subprocess.run(
                ["apk", "--root", str(sysroot), "--no-network", "info"],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            )
            self.assertEqual(
                set(installed.stdout.splitlines()), {"project-library", "unrelated-library"}
            )


if __name__ == "__main__":
    unittest.main()
