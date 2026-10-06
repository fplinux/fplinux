# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the static GLib installer with real host files and processes."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

from tests import ROOT

INSTALLER = ROOT / "alpine/aports/fplinux-glib/install-static-dev.py"


class GlibStaticInstallerTests:
    """Check required-header copying without compiling GLib or an APK."""

    def run_installer(
        self, package_dir: Path, mapping: dict[str, str], *, sysroot: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        """Run the production installer against a synthetic Meson file mapping."""
        return subprocess.run(
            [
                sys.executable,
                str(INSTALLER),
                str(package_dir),
                str(sysroot or package_dir.parent / "sysroot"),
            ],
            input=json.dumps(mapping),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_missing_unselected_introspection_header_does_not_prevent_copying_gio_header(
        self,
    ) -> None:
        """An unbuilt introspection output must not break the selected GIO interface."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            source = temporary / "gdbusconnection.h"
            source.write_text("selected GIO header\n")
            package_dir = temporary / "package"
            mapping = {
                str(source): "/usr/include/glib-2.0/gio/gdbusconnection.h",
                str(temporary / "missing-gi-visibility.h"): (
                    "/usr/include/glib-2.0/girepository/gi-visibility.h"
                ),
            }

            result = self.run_installer(package_dir, mapping)

            assert (result.returncode) == (0), result.stderr
            assert ((package_dir / "usr/include/glib-2.0/gio/gdbusconnection.h").read_text()) == (
                "selected GIO header\n"
            )
            assert not ((package_dir / "usr/include/glib-2.0/girepository").exists())

    def test_missing_selected_glib_header_fails_installation(self) -> None:
        """A missing required header must fail instead of producing an incomplete interface."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            package_dir = temporary / "package"
            mapping = {
                str(temporary / "missing-gvariant.h"): "/usr/include/glib-2.0/glib/gvariant.h"
            }

            result = self.run_installer(package_dir, mapping)

            assert (result.returncode) != (0)
            assert ("missing-gvariant.h") in (result.stderr)
            assert not ((package_dir / "usr/include/glib-2.0/glib/gvariant.h").exists())

    def test_module_and_unix_headers_are_copied_to_the_exported_include_directories(self) -> None:
        """The GIO module and Unix interfaces must retain their selected header contents."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            module_header = temporary / "gmodule-visibility.h"
            module_header.write_text("selected module visibility\n")
            unix_header = temporary / "gunixinputstream.h"
            unix_header.write_text("selected Unix stream\n")
            package_dir = temporary / "package"
            mapping = {
                str(module_header): "/usr/include/glib-2.0/gmodule/gmodule-visibility.h",
                str(unix_header): "/usr/include/gio-unix-2.0/gio/gunixinputstream.h",
            }

            result = self.run_installer(package_dir, mapping)

            assert (result.returncode) == (0), result.stderr
            assert (
                (package_dir / "usr/include/glib-2.0/gmodule/gmodule-visibility.h").read_text()
            ) == ("selected module visibility\n")
            assert (
                (package_dir / "usr/include/gio-unix-2.0/gio/gunixinputstream.h").read_text()
            ) == ("selected Unix stream\n")

    def test_atomic_archive_link_flag_does_not_retain_the_producer_sysroot(self) -> None:
        """Selected metadata must resolve the atomic library in the consuming sysroot."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            sysroot = temporary / "producer-sysroot"
            metadata = temporary / "glib-2.0.pc"
            metadata.write_text(
                "Name: GLib\n"
                f"Libs: -lglib-2.0 {sysroot}/usr/lib/libatomic.a -lm\n"
                f"Other: {sysroot}/usr/lib/libother.a {sysroot}/usr/lib/libatomic.a.backup\n"
            )
            package_dir = temporary / "package"

            result = self.run_installer(
                package_dir,
                {str(metadata): "/usr/lib/fplinux-glib-static/pkgconfig/glib-2.0.pc"},
                sysroot=sysroot,
            )

            assert (result.returncode) == (0), result.stderr
            assert ((package_dir / "usr/lib/pkgconfig/glib-2.0.pc").read_text()) == (
                "Name: GLib\n"
                "Libs: -lglib-2.0 -latomic -lm\n"
                f"Other: {sysroot}/usr/lib/libother.a {sysroot}/usr/lib/libatomic.a.backup\n"
            )
