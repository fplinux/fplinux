# SPDX-License-Identifier: GPL-2.0-only
"""Run font split functions against controlled source/package directories."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.manifests import platforms, targets

from tests import ROOT
from tests.process import run_process


class FontSplitTests(unittest.TestCase):
    """A child payload keeps its selected font and license outside the source directory."""

    def test_phone_package_selection_has_one_font_and_no_bundle_font(self) -> None:
        """Phone package selections contain one rootfs font and no bundled font."""
        cases = (
            ("inoi-240-modern-4g", "fplinux-font-terminus-6x12"),
            ("inoi-244-modern-4g", "fplinux-font-terminus-8x16"),
            ("nokia-ta1618", "fplinux-font-terminus-8x16"),
        )
        font_packages = {"fplinux-font-terminus-6x12", "fplinux-font-terminus-8x16"}
        for target, expected in cases:
            with self.subTest(target=target):
                config = targets.load_target(target)
                platform = platforms.load_platform(config["platform"])
                selected = alpine_state.selected_packages(platform, config)
                bundle = alpine_state.bundle_packages(platform, config, selected)
                self.assertEqual(set(selected) & font_packages, {expected})
                self.assertEqual(set(bundle) & font_packages, set())

    def test_font_split_moves_selected_psf_and_copies_source_license(self) -> None:
        """Each payload uses srcdir for its license and declares an unversioned data capability."""
        for cell, filename in (("6x12", "ter-u12n.psf"), ("8x16", "ter-u16n.psf")):
            with self.subTest(cell=cell), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / "source"
                package = root / "package"
                child = root / "child"
                license_path = source / "terminus-font-4.49.1/OFL.TXT"
                license_path.parent.mkdir(parents=True)
                license_path.write_bytes(b"fixture font license\n")
                font = package / "usr/share/fplinux/fonts" / filename
                font.parent.mkdir(parents=True)
                font.write_bytes(b"fixture selected PSF\n")
                other_filename = "ter-u16n.psf" if cell == "6x12" else "ter-u12n.psf"
                other_font = font.parent / other_filename
                other_font.write_bytes(b"fixture other PSF\n")
                result = run_process(
                    [
                        "sh",
                        str(ROOT / "tests/fixtures/font_split.sh"),
                        str(ROOT / "alpine/aports/fplinux-font-terminus/APKBUILD"),
                        cell,
                        str(source),
                        str(package),
                        str(child),
                    ],
                    name="split font payload",
                    timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(
                    result.stdout,
                    "depends=\nprovides=fplinux-font-terminus-data\n",
                )
                self.assertFalse(font.exists())
                self.assertEqual(other_font.read_bytes(), b"fixture other PSF\n")
                self.assertFalse((child / "usr/share/fplinux/fonts" / other_filename).exists())
                self.assertEqual(
                    (child / "usr/share/fplinux/fonts" / filename).read_bytes(),
                    b"fixture selected PSF\n",
                )
                default = child / "usr/share/fplinux/fonts/default.psf"
                self.assertTrue(default.is_symlink())
                self.assertEqual(default.read_bytes(), b"fixture selected PSF\n")
                self.assertEqual(
                    (
                        child
                        / "usr/share/licenses"
                        / f"fplinux-font-terminus-{cell}"
                        / "Terminus-OFL.txt"
                    ).read_bytes(),
                    b"fixture font license\n",
                )


if __name__ == "__main__":
    unittest.main()
