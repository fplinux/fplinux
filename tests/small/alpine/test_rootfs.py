# SPDX-License-Identifier: GPL-2.0-only
"""Alpine rootfs scenarios."""

from __future__ import annotations

import re
import subprocess
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

from tests.small.alpine.fixtures import (
    alpine_source as alpine_source,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path

    from tests.small.alpine.fixtures import AlpineSourceContext


def test_composed_rootfs_stamps_build_id_without_changing_system_version(tmp_path: Path) -> None:
    """Image composition adds the selected ID and replaces any previous stamped value."""
    release = tmp_path / "etc/os-release"
    release.parent.mkdir()
    release.write_bytes(b'NAME=FPLinux\nVERSION_ID=1\nBUILD_ID="old"\n')
    alpine_rootfs_files._stamp_build_id(tmp_path, "012345abcdef")  # noqa: SLF001 -- inspect composed image bytes.
    assert release.read_bytes() == b"NAME=FPLinux\nVERSION_ID=1\nBUILD_ID=012345abcdef\n"
    alpine_rootfs_files._stamp_build_id(tmp_path, "abcdef012345")  # noqa: SLF001 -- compose the next release.
    assert release.read_bytes() == b"NAME=FPLinux\nVERSION_ID=1\nBUILD_ID=abcdef012345\n"


def verified_rootfs(alpine_source: AlpineSourceContext) -> Path:
    """Create the smallest root tree accepted without optional packages."""
    root = alpine_source.directory / "verified-rootfs"
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
    (root / "etc/init.d/fplinux-brightness").write_text("#!/bin/sh\n", encoding="utf-8")
    (root / "usr/bin/fplinux-brightness").write_text("brightness\n", encoding="utf-8")
    (root / "usr/libexec/fplinux").mkdir(parents=True)
    (root / "usr/libexec/fplinux/brightnessd").write_text("brightnessd\n", encoding="utf-8")
    (root / "etc/runlevels/default/fplinux-brightness").symlink_to(
        "/etc/init.d/fplinux-brightness"
    )
    (root / "init").symlink_to("/sbin/init")
    return root


def write_world(root: Path, packages: tuple[str, ...]) -> None:
    """Select the packages installed into one prepared root tree."""
    (root / "etc/apk").mkdir(parents=True, exist_ok=True)
    (root / "etc/apk/world").write_text(
        "\n".join(packages) + "\n",
        encoding="utf-8",
    )


def install_graphical_session(root: Path) -> None:
    """Provide the graphical session's runtime files and enabled service."""
    for relative in (
        "etc/init.d/fplinux-session",
        "usr/bin/fplinux-session",
        "usr/bin/weston",
        "usr/lib/weston/fplinux-shell.so",
        "etc/fplinux/weston.ini",
    ):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("graphical session fixture\n", encoding="utf-8")
    (root / "etc/runlevels/default/fplinux-session").symlink_to("/etc/init.d/fplinux-session")


def fake_apk_owner(owners: dict[str, str]) -> Callable[[Path, str, str], None]:
    """Fake apk file-ownership answers; paths outside ``owners`` always match."""

    def fake_apk_owner(_root: Path, path: str, package: str) -> None:
        owner = owners.get(path, package)
        if owner != package:
            raise SystemExit(f"unexpected Alpine package owner for {path}: {owner}")

    return fake_apk_owner


class AlpineRootfsTests:
    """Protect Alpine rootfs behavior with controlled temporary inputs."""

    def test_graphical_rootfs_accepts_its_compositor_and_startup_service(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """The selected graphical session has its runtime and enabled service."""
        root = verified_rootfs(alpine_source)
        install_graphical_session(root)
        packages = ("fplinux-base", "fplinux-session")
        write_world(root, packages)

        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    @pytest.mark.parametrize(
        "missing",
        [
            "usr/bin/fplinux-session",
            "usr/bin/weston",
            "usr/lib/weston/fplinux-shell.so",
            "etc/fplinux/weston.ini",
            "etc/init.d/fplinux-session",
            "etc/runlevels/default/fplinux-session",
        ],
        ids=["command", "compositor", "shell", "configuration", "service", "startup"],
    )
    def test_graphical_rootfs_requires_its_runtime_and_startup_service(
        self, alpine_source: AlpineSourceContext, missing: str
    ) -> None:
        """A selected graphical session must have the files used to start its compositor."""
        root = verified_rootfs(alpine_source)
        install_graphical_session(root)
        packages = ("fplinux-base", "fplinux-session")
        write_world(root, packages)
        (root / missing).unlink()

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            pytest.raises(SystemExit, match=re.escape((root / missing).name)),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_graphical_rootfs_requires_the_session_openrc_package_owner(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """The enabled service must be supplied by the graphical OpenRC package."""
        root = verified_rootfs(alpine_source)
        install_graphical_session(root)
        packages = ("fplinux-base", "fplinux-session")
        write_world(root, packages)
        owners = {"/etc/init.d/fplinux-session": "fplinux-base"}

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner(owners)),
            pytest.raises(SystemExit, match="fplinux-session: fplinux-base"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    @pytest.mark.parametrize(
        "missing",
        [
            None,
            "usr/bin/fplinux-terminal",
            "usr/libexec/fplinux-terminal-pty",
            "etc/fplinux/terminal.bashrc",
            "usr/share/applications/org.fplinux.terminal.desktop",
        ],
        ids=["complete", "application", "pty-helper", "shell-startup", "catalog-entry"],
    )
    def test_terminal_rootfs_requires_its_application_shell_and_catalog_entry(
        self, alpine_source: AlpineSourceContext, missing: str | None
    ) -> None:
        """The selected terminal has the files used to discover and start its shell."""
        root = verified_rootfs(alpine_source)
        packages: tuple[str, ...] = ("fplinux-base", "fplinux-terminal")
        write_world(root, packages)
        for relative in (
            "usr/bin/fplinux-terminal",
            "usr/libexec/fplinux-terminal-pty",
            "etc/fplinux/terminal.bashrc",
            "usr/share/applications/org.fplinux.terminal.desktop",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("terminal fixture\n", encoding="utf-8")

        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            if missing is None:
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            else:
                (root / missing).unlink()
                with pytest.raises(SystemExit, match=re.escape((root / missing).name)):
                    alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_materializing_a_subpackage_copies_the_producer_sources(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A child name stages its existing producer instead of requiring a new source tree."""
        alpine_source.write("alpine/aports/fplinux-ncurses/APKBUILD", b"pkgname=fplinux-ncurses\n")
        alpine_source.write("alpine/aports/fplinux-ncurses/adapter.c", b"producer adapter\n")
        destination = alpine_source.root / "stage"

        alpine_aports.materialize_aport_sources(
            "fplinux-ncurses-curses", alpine_source.root, destination
        )

        assert ((destination / "APKBUILD").read_bytes()) == (b"pkgname=fplinux-ncurses\n")
        assert ((destination / "adapter.c").read_bytes()) == (b"producer adapter\n")

    def test_bundle_absence_check_interprets_mocked_apk_exit_codes(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Map mocked ``apk info --exists`` results to absent and installed outcomes."""
        root = alpine_source.directory / "rootfs"
        package = alpine_source.packages[1]
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

    def test_bundle_install_check_interprets_mocked_apk_simulation(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A mocked offline ``apk add --simulate`` result accepts or rejects the bundle APKs."""
        root = verified_rootfs(alpine_source)
        base = ("fplinux-base",)
        bundle_apk = alpine_source.write("built/fplinux-package-b.apk", b"bundle\n")
        unresolved = (
            "ERROR: unable to select packages:\n"
            "  so:libexample.so.1 (no such package):\n"
            "    required by: fplinux-package-b-1-r0[so:libexample.so.1]\n"
        )
        write_world(root, base)

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

    def test_brightness_config_has_exact_runtime_bytes_and_is_verified(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Composition writes the target table and rejects changed installed bytes."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base",)
        write_world(root, packages)
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

    def test_rootfs_without_brightness_table_has_no_brightness_config(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A headless target does not receive display configuration."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base",)
        write_world(root, packages)
        alpine_rootfs_files._install_display_brightness(root, None)  # noqa: SLF001
        config = root / "etc/fplinux/brightness.conf"
        assert not (config.exists())
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001
            config.parent.mkdir(parents=True)
            config.write_text("backlight=unexpected\nlevels=0,1,2,3,4,5,6,7,8,9,10\n")
            with pytest.raises(SystemExit, match="without a target table"):
                alpine_rootfs_verify._verify_alpine_rootfs(root, packages)  # noqa: SLF001

    def test_rootfs_requires_brightness_command_and_startup_service(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """The composed base rootfs must provide the command and its default runlevel."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base",)
        write_world(root, packages)
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

    def test_rootfs_verifier_requires_input_files_only_when_selected(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A gadgetless root accepts no input bridge, while the selected bridge is required."""
        root = verified_rootfs(alpine_source)
        without_input = ("fplinux-base",)
        write_world(root, without_input)
        with mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"):
            alpine_rootfs_verify._verify_alpine_rootfs(root, without_input)  # noqa: SLF001

        with_input = (*without_input, "fplinux-input")
        write_world(root, with_input)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner"),
            pytest.raises(SystemExit, match="fplinux-input"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_input)  # noqa: SLF001

    def test_rootfs_verifier_rejects_disabled_boot_networking(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A root lacking network startup must be rejected before publication."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base",)
        write_world(root, packages)
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
        self, alpine_source: AlpineSourceContext, path: str, alpine: str
    ) -> None:
        """The selected Bluetooth root rejects stock daemon and GLib replacements."""
        root = verified_rootfs(alpine_source)
        base = ("fplinux-base",)
        with_bluetooth = (*base, "fplinux-bluetooth")
        replaced = {
            "/usr/lib/bluetooth/obexd": ("bluez-obexd", "fplinux-bluez"),
            "/usr/lib/libfplinux-bluez-glib-2.0.so.0": ("glib", "fplinux-bluez-glib"),
        }
        project_owners = {path: project for path, (_, project) in replaced.items()}

        write_world(root, base)
        alpine_owners = {path: alpine for path, (alpine, _) in replaced.items()}
        with mock.patch.object(
            alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner(alpine_owners)
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base)  # noqa: SLF001

        write_world(root, with_bluetooth)
        owners = {**project_owners, path: alpine}
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner(owners)),
            pytest.raises(SystemExit, match=f"{re.escape(path)}: {alpine}"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_bluetooth)  # noqa: SLF001

        with mock.patch.object(
            alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner(project_owners)
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
    def test_replaced_package_manager_root_rejects_openssl_leftovers(
        self, alpine_source: AlpineSourceContext, leftover: str
    ) -> None:
        """A root without dependent tools must replace the package manager and drop OpenSSL."""
        root = verified_rootfs(alpine_source)
        base = ("fplinux-base",)
        with_apk_tools = (*base, "fplinux-apk-tools")
        installed: set[str] = {"apk-tools", "libapk", "libcrypto3", "libssl3", "ssl_client"}
        minirootfs_owner = fake_apk_owner({"/sbin/apk": "apk-tools"})

        def fake_installed(_root: Path, package: str) -> bool:
            return package in installed

        write_world(root, base)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, base)  # noqa: SLF001

        write_world(root, with_apk_tools)
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", minirootfs_owner),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match="/sbin/apk: apk-tools"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        installed = {leftover}
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner({})),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match=f"remains in the rootfs: {leftover}"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

        installed = set()
        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner({})),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(root, with_apk_tools)  # noqa: SLF001

    def test_rootfs_accepts_openssl_required_by_selected_wifi(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """An explicit Wi-Fi dependency can coexist with the Mbed TLS package manager."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base", "fplinux-apk-tools", "fplinux-wifi")
        installed = {"libcrypto3", "libssl3"}
        write_world(root, packages)

        def fake_installed(_root: Path, package: str) -> bool:
            """Fake the APK database for the selected tools' shared libraries."""
            return package in installed

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner({})),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(  # noqa: SLF001
                root,
                packages,
                runtime_packages=("libcrypto3-3.5.9-r0.apk", "libssl3-3.5.9-r0.apk"),
            )

    @pytest.mark.parametrize(
        "leftover",
        [pytest.param("apk-tools", id="apk-tools"), pytest.param("libapk", id="libapk")],
    )
    def test_rootfs_rejects_replaced_package_manager_with_declared_openssl(
        self, alpine_source: AlpineSourceContext, leftover: str
    ) -> None:
        """Declared OpenSSL dependencies never permit the replaced package manager."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base", "fplinux-apk-tools", "fplinux-wifi")
        installed = {leftover, "libcrypto3", "libssl3"}
        write_world(root, packages)

        def fake_installed(_root: Path, package: str) -> bool:
            """Fake the APK database containing one replaced package-manager component."""
            return package in installed

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner({})),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match=f"remains in the rootfs: {leftover}"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(  # noqa: SLF001
                root,
                packages,
                runtime_packages=("libcrypto3-3.5.9-r0.apk", "libssl3-3.5.9-r0.apk"),
            )

    def test_rootfs_rejects_openssl_library_outside_selected_runtime(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """Selecting libcrypto does not authorize an unrelated leftover libssl package."""
        root = verified_rootfs(alpine_source)
        packages = ("fplinux-base", "fplinux-apk-tools")
        installed = {"libcrypto3", "libssl3"}
        write_world(root, packages)

        def fake_installed(_root: Path, package: str) -> bool:
            """Fake the APK database containing one required and one unneeded library."""
            return package in installed

        with (
            mock.patch.object(alpine_rootfs_verify, "_require_apk_owner", fake_apk_owner({})),
            mock.patch.object(alpine_rootfs_verify, "_alpine_package_installed", fake_installed),
            pytest.raises(SystemExit, match="remains in the rootfs: libssl3"),
        ):
            alpine_rootfs_verify._verify_alpine_rootfs(  # noqa: SLF001
                root, packages, runtime_packages=("libcrypto3-3.5.9-r0.apk",)
            )

    def test_persistent_root_requires_orderly_shutdown_services(
        self, alpine_source: AlpineSourceContext
    ) -> None:
        """A persistent root rejects a shutdown runlevel missing data-safety services."""
        root = verified_rootfs(alpine_source)
        without_input = ("fplinux-base",)
        microsd_root = (*without_input, "fplinux-microsd-root")
        write_world(root, microsd_root)
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
