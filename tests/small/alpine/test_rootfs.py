# SPDX-License-Identifier: GPL-2.0-only
"""Alpine rootfs scenarios."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.alpine import (
    aports as alpine_aports,
)
from fplinux_cli.alpine import (
    rootfs_files as alpine_rootfs_files,
)
from fplinux_cli.alpine import (
    rootfs_verify as alpine_rootfs_verify,
)

from tests.small.alpine import fixtures

if TYPE_CHECKING:
    from collections.abc import Callable


class AlpineRootfsTests(fixtures.AlpineSourceFixture):
    """Protect Alpine rootfs behavior with controlled temporary inputs."""

    def _verified_rootfs(self) -> Path:
        """Create the smallest root tree accepted without optional packages."""
        root = Path(self.temporary.name) / "verified-rootfs"
        (root / "etc/init.d").mkdir(parents=True)
        (root / "etc/runlevels/default").mkdir(parents=True)
        (root / "etc/runlevels/boot").mkdir(parents=True)
        (root / "etc/network").mkdir(parents=True)
        (root / "usr/bin").mkdir(parents=True)
        (root / "etc/fstab").write_text(
            "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0\n",
            encoding="utf-8",
        )
        (root / "etc/inittab").write_text("::sysinit:/sbin/openrc sysinit\n", encoding="utf-8")
        (root / "etc/os-release").write_text("NAME=FPLinux\n", encoding="utf-8")
        (root / "etc/network/interfaces").write_text(
            "auto lo\niface lo inet loopback\n", encoding="utf-8"
        )
        (root / "etc/init.d/networking").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "etc/runlevels/boot/networking").symlink_to("/etc/init.d/networking")
        (root / "etc/init.d/fplinux-terminal").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "etc/init.d/fplinux-brightness").write_text("#!/bin/sh\n", encoding="utf-8")
        (root / "usr/bin/fplinux-terminal").write_text("terminal\n", encoding="utf-8")
        (root / "usr/bin/fplinux-brightness").write_text("brightness\n", encoding="utf-8")
        (root / "usr/libexec/fplinux").mkdir(parents=True)
        (root / "usr/libexec/fplinux/brightnessd").write_text("brightnessd\n", encoding="utf-8")
        (root / "etc/runlevels/default/fplinux-terminal").symlink_to(
            "/etc/init.d/fplinux-terminal"
        )
        (root / "etc/runlevels/default/fplinux-brightness").symlink_to(
            "/etc/init.d/fplinux-brightness"
        )
        (root / "init").symlink_to("/sbin/init")
        return root

    @staticmethod
    def _write_world(root: Path, packages: tuple[str, ...]) -> None:
        """Select the packages installed into one prepared root tree."""
        (root / "etc/apk").mkdir(parents=True, exist_ok=True)
        (root / "etc/apk/world").write_text(
            "\n".join(packages) + "\n",
            encoding="utf-8",
        )

    @staticmethod
    def _fake_apk_owner(owners: dict[str, str]) -> Callable[[Path, str, str], None]:
        """Fake apk file-ownership answers; paths outside ``owners`` always match."""

        def fake_apk_owner(_root: Path, path: str, package: str) -> None:
            owner = owners.get(path, package)
            if owner != package:
                raise SystemExit(f"unexpected Alpine package owner for {path}: {owner}")

        return fake_apk_owner

    def test_materializing_a_subpackage_copies_the_producer_sources(self) -> None:
        """A child name stages its existing producer instead of requiring a new source tree."""
        self._write("alpine/aports/fplinux-ncurses/APKBUILD", b"pkgname=fplinux-ncurses\n")
        self._write("alpine/aports/fplinux-ncurses/adapter.c", b"producer adapter\n")
        destination = self.root / "stage"

        alpine_aports.materialize_aport_sources("fplinux-ncurses-curses", self.root, destination)

        assert ((destination / "APKBUILD").read_bytes()) == (b"pkgname=fplinux-ncurses\n")
        assert ((destination / "adapter.c").read_bytes()) == (b"producer adapter\n")

    def test_bundle_absence_check_interprets_mocked_apk_exit_codes(self) -> None:
        """Map mocked ``apk info --exists`` results to absent and installed outcomes."""
        root = Path(self.temporary.name) / "rootfs"
        package = self.packages[1]
        with mock.patch.object(
            subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, "", ""),
        ):
            alpine_rootfs_verify._require_bundle_package_absent(root, package)  # noqa: SLF001
        with (
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, f"{package}\n", ""),
            ),
            pytest.raises(SystemExit, match="installed in the standard rootfs"),
        ):
            alpine_rootfs_verify._require_bundle_package_absent(root, package)  # noqa: SLF001
        with (
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 2, "", "apk failed"),
            ),
            pytest.raises(SystemExit, match=r"cannot verify.*apk failed"),
        ):
            alpine_rootfs_verify._require_bundle_package_absent(root, package)  # noqa: SLF001

    def test_bundle_install_check_interprets_mocked_apk_simulation(self) -> None:
        """A mocked offline ``apk add --simulate`` result accepts or rejects the bundle APKs."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        bundle_apk = self._write("built/fplinux-package-b.apk", b"bundle\n")
        unresolved = (
            "ERROR: unable to select packages:\n"
            "  so:libexample.so.1 (no such package):\n"
            "    required by: fplinux-package-b-1-r0[so:libexample.so.1]\n"
        )
        self._write_world(root, base)

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            mock.patch.object(
                subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")
            ),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base, bundle_apks=(bundle_apk,))  # noqa: SLF001

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            mock.patch.object(
                subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 7, "", unresolved),
            ),
            pytest.raises(SystemExit) as rejected,
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base, bundle_apks=(bundle_apk,))  # noqa: SLF001
        assert ("cannot be installed into the rootfs offline") in (str(rejected.value))
        assert ("so:libexample.so.1 (no such package)") in (str(rejected.value))

    def test_brightness_config_has_exact_runtime_bytes_and_is_verified(self) -> None:
        """Composition writes the target table and rejects changed installed bytes."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        table = {"backlight": "screen-backlight", "levels": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}
        config = root / "etc/fplinux/brightness.conf"

        alpine_rootfs_files._install_display_brightness(root, table)  # noqa: SLF001
        assert (config.read_bytes()) == (
            b"backlight=screen-backlight\nlevels=0,1,2,3,4,5,6,7,8,9,10\n"
        )
        assert (config.stat().st_mode & 0o777) == (0o644)
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(  # noqa: SLF001
                root, packages, display_brightness=table
            )
            config.write_text("backlight=other-backlight\nlevels=0,1,2,3,4,5,6,7,8,9,10\n")
            with pytest.raises(SystemExit, match="brightness configuration does not match"):
                alpine_rootfs_verify._verify_alpine_rootfs(  # noqa: SLF001
                    root, packages, display_brightness=table
                )

    def test_rootfs_without_brightness_table_has_no_brightness_config(self) -> None:
        """A headless target does not receive display configuration."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        alpine_rootfs_files._install_display_brightness(root, None)  # noqa: SLF001
        config = root / "etc/fplinux/brightness.conf"
        assert not (config.exists())
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            config.parent.mkdir(parents=True)
            config.write_text("backlight=unexpected\nlevels=0,1,2,3,4,5,6,7,8,9,10\n")
            with pytest.raises(SystemExit, match="without a target table"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_rootfs_requires_brightness_command_and_startup_service(self) -> None:
        """The composed base rootfs must provide the command and its default runlevel."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        command = root / "usr/bin/fplinux-brightness"
        daemon = root / "usr/libexec/fplinux/brightnessd"
        service = root / "etc/runlevels/default/fplinux-brightness"

        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            command.unlink()
            with pytest.raises(SystemExit, match="fplinux-brightness"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            command.write_text("brightness\n", encoding="utf-8")
            daemon.unlink()
            with pytest.raises(SystemExit, match="brightnessd"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            daemon.write_text("brightnessd\n", encoding="utf-8")
            service.unlink()
            with pytest.raises(SystemExit, match="fplinux-brightness"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_rootfs_verifier_requires_input_files_only_when_selected(self) -> None:
        """A gadgetless root accepts no input bridge, while the selected bridge is required."""
        root = self._verified_rootfs()
        without_input = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, without_input)
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, without_input)  # noqa: SLF001

        with_input = (*without_input, "fplinux-input")
        self._write_world(root, with_input)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            pytest.raises(SystemExit, match="fplinux-input"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_input)  # noqa: SLF001

    def test_rootfs_verifier_rejects_disabled_boot_networking(self) -> None:
        """A root lacking network startup must be rejected before publication."""
        root = self._verified_rootfs()
        packages = ("fplinux-base", "fplinux-terminal")
        self._write_world(root, packages)
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            (root / "etc/runlevels/boot/networking").unlink()
            with pytest.raises(SystemExit, match="boot runlevel is missing networking"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    @pytest.mark.parametrize(
        ("path", "alpine"),
        [
            pytest.param("/usr/lib/bluetooth/obexd", "bluez-obexd", id="daemon"),
            pytest.param("/usr/lib/libfplinux-bluez-glib-2.0.so.0", "glib", id="glib-library"),
        ],
    )
    def test_bluetooth_root_requires_project_daemon_and_library_owners(
        self, path: str, alpine: str
    ) -> None:
        """The selected Bluetooth root rejects stock daemon and GLib replacements."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        with_bluetooth = (*base, "fplinux-bluetooth")
        replaced = {
            "/usr/lib/bluetooth/obexd": ("bluez-obexd", "fplinux-bluez"),
            "/usr/lib/libfplinux-bluez-glib-2.0.so.0": ("glib", "fplinux-bluez-glib"),
        }
        project_owners = {path: project for path, (_, project) in replaced.items()}

        self._write_world(root, base)
        alpine_owners = {path: alpine for path, (alpine, _) in replaced.items()}
        with mock.patch.object(
            alpine_rootfs_verify, "_require_apk_owner", self._fake_apk_owner(alpine_owners)
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base)  # noqa: SLF001

        self._write_world(root, with_bluetooth)
        owners = {**project_owners, path: alpine}
        with (
            mock.patch.object(
                alpine_rootfs_verify, "_require_apk_owner", self._fake_apk_owner(owners)
            ),
            pytest.raises(SystemExit, match=f"{re.escape(path)}: {alpine}"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_bluetooth)  # noqa: SLF001

        with mock.patch.object(
            alpine_rootfs_verify, "_require_apk_owner", self._fake_apk_owner(project_owners)
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_bluetooth)  # noqa: SLF001

    @pytest.mark.parametrize(
        "leftover",
        [
            pytest.param("apk-tools", id="apk-tools"),
            pytest.param("libapk", id="libapk"),
            pytest.param("libcrypto3", id="libcrypto3"),
            pytest.param("libssl3", id="libssl3"),
        ],
    )
    def test_replaced_package_manager_root_rejects_openssl_leftovers(self, leftover: str) -> None:
        """A root with the Mbed TLS apk must own /sbin/apk and hold no OpenSSL packages."""
        root = self._verified_rootfs()
        base = ("fplinux-base", "fplinux-terminal")
        with_apk_tools = (*base, "fplinux-apk-tools")
        installed: set[str] = {"apk-tools", "libapk", "libcrypto3", "libssl3", "ssl_client"}
        minirootfs_owner = self._fake_apk_owner({"/sbin/apk": "apk-tools"})

        def fake_installed(_root: Path, package: str) -> bool:
            return package in installed

        self._write_world(root, base)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base)  # noqa: SLF001

        self._write_world(root, with_apk_tools)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match="/sbin/apk: apk-tools"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        installed = {leftover}
        with (
            mock.patch.object(
                alpine_rootfs_verify, "_require_apk_owner", self._fake_apk_owner({})
            ),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match=f"remains in the rootfs: {leftover}"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        installed = set()
        with (
            mock.patch.object(
                alpine_rootfs_verify, "_require_apk_owner", self._fake_apk_owner({})
            ),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

    def test_persistent_root_requires_orderly_shutdown_services(self) -> None:
        """A persistent root rejects a shutdown runlevel missing data-safety services."""
        root = self._verified_rootfs()
        without_input = ("fplinux-base", "fplinux-terminal")
        microsd_root = (*without_input, "fplinux-microsd-root")
        self._write_world(root, microsd_root)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            pytest.raises(SystemExit, match="shutdown runlevel is missing killprocs"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, microsd_root)  # noqa: SLF001
        shutdown = root / "etc/runlevels/shutdown"
        shutdown.mkdir(parents=True)
        for service in ("killprocs", "savecache", "mount-ro"):
            (shutdown / service).symlink_to(f"/etc/init.d/{service}")
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, microsd_root)  # noqa: SLF001
