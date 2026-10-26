# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem checks for installing captured offline quality sources."""

from __future__ import annotations

import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest
from fplinux_cli.environment import install

_FIXTURES = Path(__file__).parents[2] / "fixtures/environment/terminal-font"


def test_installed_quality_sources_preserve_ui_fonts_licenses_and_uploader(tmp_path: Path) -> None:
    """Prepared environments retain the captured bytes needed by network-free rendering."""
    resources = tmp_path / "resources"
    fonts = resources / "quality/ui-fonts"
    fonts.mkdir(parents=True)
    (fonts / "face.ttf").write_bytes(b"font fixture\n")
    (fonts / "face.ttf").chmod(0o600)
    (fonts / "OFL.txt").write_bytes(b"font license\n")
    icons = resources / "quality/ui-icons"
    icons.mkdir()
    (icons / "tabler.tgz").write_bytes(b"icon archive fixture\n")
    uploader = resources / "quality/spreadtrum-flash"
    uploader.mkdir()
    (uploader / "spd_dump.c").write_bytes(b"uploader fixture\n")
    destination = tmp_path / "rootfs/opt/quality/src"
    destination.mkdir(parents=True)
    (destination / "lvgl.tar.gz").write_bytes(b"existing tool source\n")

    install._install_quality_sources(resources, destination)  # noqa: SLF001 -- exercise source installation without unrelated system mutations.

    assert (destination / "ui-fonts/face.ttf").read_bytes() == b"font fixture\n"
    assert (destination / "ui-fonts/face.ttf").stat().st_mode & 0o777 == 0o644
    assert (destination / "ui-fonts/OFL.txt").read_bytes() == b"font license\n"
    assert (destination / "ui-icons/tabler.tgz").read_bytes() == b"icon archive fixture\n"
    assert (destination / "ui-icons/tabler.tgz").stat().st_mode & 0o777 == 0o644
    assert (destination / "spreadtrum-flash/spd_dump.c").read_bytes() == b"uploader fixture\n"
    assert (destination / "lvgl.tar.gz").read_bytes() == b"existing tool source\n"


def _terminal_font_inputs(
    tmp_path: Path,
    *,
    missing_input: str | None = None,
) -> tuple[Path, Path, Path]:
    resources = tmp_path / "resources"
    archive = resources / "quality/terminus/terminus-font-1.tar.gz"
    archive.parent.mkdir(parents=True)
    with tarfile.open(archive, "w:gz") as bundle:
        for source, filename in (
            ("terminal.psf.in", "terminal.psf.in"),
            ("terminal-8x16.psf.in", "terminal-8x16.psf.in"),
            ("OFL.TXT.fixture", "OFL.TXT"),
        ):
            if filename != missing_input:
                bundle.add(_FIXTURES / source, arcname=f"terminus-font-1/{filename}")
    aport = resources / "quality/terminus-aport/APKBUILD"
    aport.parent.mkdir(parents=True)
    shutil.copyfile(_FIXTURES / "APKBUILD.fixture", aport)
    temporary = tmp_path / "temporary"
    temporary.mkdir()
    destination = tmp_path / "rootfs/opt/quality/fonts"
    return resources, temporary, destination


def test_terminal_font_install_runs_captured_package_build_and_keeps_license(
    tmp_path: Path,
) -> None:
    """Both offline fonts come from the captured package recipe and retain their license."""
    resources, temporary, destination = _terminal_font_inputs(tmp_path)

    install._install_terminal_font(resources, temporary, destination)  # noqa: SLF001 -- exercise font installation without unrelated system mutations.

    assert (destination / "ter-u12n-terminal.psf").read_bytes() == b"terminal font fixture\n"
    assert (destination / "ter-u12n-terminal.psf").stat().st_mode & 0o777 == 0o644
    assert (destination / "ter-u16n-terminal.psf").read_bytes() == b"terminal 8x16 font fixture\n"
    assert (destination / "ter-u16n-terminal.psf").stat().st_mode & 0o777 == 0o644
    assert (destination / "Terminus-OFL.txt").read_bytes() == b"font license fixture\n"
    assert (destination / "Terminus-OFL.txt").stat().st_mode & 0o777 == 0o644


@pytest.mark.parametrize("missing_input", ["terminal-8x16.psf.in", "OFL.TXT"])
def test_terminal_font_install_propagates_missing_build_or_license_input(
    tmp_path: Path, missing_input: str
) -> None:
    """Incomplete captured inputs fail preparation instead of reporting installed fonts."""
    resources, temporary, destination = _terminal_font_inputs(
        tmp_path, missing_input=missing_input
    )

    with pytest.raises(subprocess.CalledProcessError) as error:
        install._install_terminal_font(resources, temporary, destination)  # noqa: SLF001 -- exercise font installation without unrelated system mutations.

    assert error.value.returncode != 0
    assert not (destination / "Terminus-OFL.txt").exists()
