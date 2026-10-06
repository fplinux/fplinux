# SPDX-License-Identifier: GPL-2.0-only
"""Run font split functions against controlled source/package directories."""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest
from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.manifests import platforms, targets

from tests import ROOT
from tests.process import run_process


class FontSplitTests:
    """A child payload keeps its selected font and license outside the source directory."""

    @pytest.mark.parametrize(
        ("target", "expected"),
        [
            ("inoi-240-modern-4g", "fplinux-font-terminus-6x12"),
            ("inoi-244-modern-4g", "fplinux-font-terminus-8x16"),
            ("nokia-ta1618", "fplinux-font-terminus-8x16"),
        ],
        ids=["inoi-240-modern-4g", "inoi-244-modern-4g", "nokia-ta1618"],
    )
    def test_phone_package_selection_has_one_font_and_no_bundle_font(
        self, target: str, expected: str
    ) -> None:
        """Phone package selections contain one rootfs font and no bundled font."""
        font_packages = {"fplinux-font-terminus-6x12", "fplinux-font-terminus-8x16"}
        config = targets.load_target(target)
        platform = platforms.load_platform(config["platform"])
        selected = alpine_state.selected_packages(platform, config)
        bundle = alpine_state.bundle_packages(platform, config, selected)
        assert (set(selected) & font_packages) == ({expected})
        assert (set(bundle) & font_packages) == (set())

    @pytest.mark.parametrize(
        ("cell", "filename"),
        [("6x12", "ter-u12n.psf"), ("8x16", "ter-u16n.psf")],
        ids=["6x12-ter-u12n.psf", "8x16-ter-u16n.psf"],
    )
    def test_font_split_moves_selected_psf_and_copies_source_license(
        self, cell: str, filename: str
    ) -> None:
        """Each payload uses srcdir for its license and declares an unversioned data capability."""
        with tempfile.TemporaryDirectory() as temporary:
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
            assert (result.returncode) == (0), result.stdout + result.stderr
            assert (result.stdout) == ("depends=\nprovides=fplinux-font-terminus-data\n")
            assert not (font.exists())
            assert (other_font.read_bytes()) == (b"fixture other PSF\n")
            assert not ((child / "usr/share/fplinux/fonts" / other_filename).exists())
            assert ((child / "usr/share/fplinux/fonts" / filename).read_bytes()) == (
                b"fixture selected PSF\n"
            )
            default = child / "usr/share/fplinux/fonts/default.psf"
            assert default.is_symlink()
            assert (default.read_bytes()) == (b"fixture selected PSF\n")
            assert (
                (
                    child
                    / "usr/share/licenses"
                    / f"fplinux-font-terminus-{cell}"
                    / "Terminus-OFL.txt"
                ).read_bytes()
            ) == (b"fixture font license\n")
