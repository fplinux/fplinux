# SPDX-License-Identifier: GPL-2.0-only
"""Verify a composed Alpine root filesystem and its offline bundle packages."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.build import process as process_build
from fplinux_cli.build.environment import build_environment
from fplinux_cli.build.inputs import require_file
from fplinux_cli.common import fail
from fplinux_cli.device_data import inputs as firmware_inputs

from .rootfs_files import _brightness_config_text

if TYPE_CHECKING:
    from collections.abc import Sequence


def _require_apk_owner(root: Path, path: str, package: str) -> None:
    result = subprocess.run(
        ["apk", "--root", str(root), "--no-network", "info", "-W", path],
        capture_output=True,
        text=True,
        check=False,
        env=build_environment(),
    )
    expected = f"{path} is owned by {package}-"
    if result.returncode != 0 or not result.stdout.strip().startswith(expected):
        detail = result.stderr.strip() or result.stdout.strip() or "no APK owner reported"
        fail(f"unexpected Alpine package owner for {path}: {detail}")


def _alpine_package_installed(root: Path, package: str) -> bool:
    result = subprocess.run(
        ["apk", "--root", str(root), "--no-network", "info", "--exists", package],
        capture_output=True,
        text=True,
        check=False,
        env=build_environment(),
    )
    if result.returncode not in {0, 1}:
        detail = result.stderr.strip() or result.stdout.strip() or "no APK diagnostic"
        fail(f"cannot verify whether Alpine package is installed: {package}: {detail}")
    return result.returncode == 0


def _require_bundle_package_absent(root: Path, package: str) -> None:
    if _alpine_package_installed(root, package):
        fail(f"bundle Alpine package was installed in the standard rootfs: {package}")


def _require_bundle_packages_installable(root: Path, bundle_apks: Sequence[Path]) -> None:
    """Require the published bundle APKs to install offline into this exact root.

    The phone installs them with the documented ``apk add --no-network
    --allow-untrusted --force-non-repository`` command and has no package
    repository, so every dependency must already be provided by the composed
    root or by another bundle APK rather than by an incidental Alpine closure.
    """
    if not bundle_apks:
        return
    result = subprocess.run(
        [
            "apk",
            "--root",
            str(root),
            "--no-network",
            "--repositories-file",
            "/dev/null",
            "--allow-untrusted",
            "--force-non-repository",
            "--simulate",
            "add",
            *(str(require_file(apk)) for apk in bundle_apks),
        ],
        capture_output=True,
        text=True,
        check=False,
        env=build_environment(),
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no APK diagnostic"
        fail(f"bundle Alpine packages cannot be installed into the rootfs offline: {detail}")


def _require_cached_bundle_packages_installable(rootfs: Path, bundle_apks: Sequence[Path]) -> None:
    """Check the cached root's package database without recomposing its files."""
    if not bundle_apks:
        return
    with tempfile.TemporaryDirectory(prefix="fplinux-apk-check-") as temporary:
        root = Path(temporary)
        process_build.run(
            [
                "cpio",
                "--extract",
                "--quiet",
                "--make-directories",
                "--no-preserve-owner",
                "--file",
                str(rootfs),
                "--directory",
                str(root),
                "etc/apk/*",
                "lib/apk/db/*",
            ]
        )
        _require_bundle_packages_installable(root, bundle_apks)


def _require_openrc_service(root: Path, runlevel: str, service: str) -> None:
    link = root / "etc/runlevels" / runlevel / service
    if not link.is_symlink() or link.readlink() != Path(f"/etc/init.d/{service}"):
        fail(f"Alpine rootfs {runlevel} runlevel is missing {service}")


def _verify_alpine_rootfs(  # noqa: PLR0913 -- verify each independently selected rootfs input.
    root: Path,
    packages: tuple[str, ...],
    bundle_packages: tuple[str, ...] = (),
    firmware: Sequence[firmware_inputs.FirmwareInput] = (),
    *,
    bundle_apks: Sequence[Path] = (),
    display_brightness: dict[str, Any] | None = None,
) -> None:
    init = root / "init"
    if not init.is_symlink() or init.readlink() != Path("/sbin/init"):
        fail("Alpine rootfs /init must point to /sbin/init")

    owners = {
        "/etc/fstab": "fplinux-base",
        "/etc/inittab": "fplinux-base",
        "/etc/network/interfaces": "fplinux-base",
        "/etc/init.d/networking": "fplinux-openrc",
        "/etc/os-release": "fplinux-base",
        "/etc/init.d/fplinux-brightness": "fplinux-base",
        "/usr/bin/fplinux-brightness": "fplinux-base",
        "/usr/libexec/fplinux/brightnessd": "fplinux-base",
        "/etc/init.d/fplinux-terminal": "fplinux-terminal-openrc",
        "/usr/bin/fplinux-terminal": "fplinux-terminal",
    }
    if "fplinux-input" in packages:
        owners.update(
            {
                "/etc/init.d/fplinux-input": "fplinux-input-openrc",
                "/usr/bin/fplinux-input": "fplinux-input",
            }
        )
    if "fplinux-cpuclock" in packages:
        owners["/usr/bin/fplinux-cpuclock"] = "fplinux-cpuclock"
    if "fplinux-tyrquake" in packages:
        owners["/usr/bin/quake"] = "fplinux-tyrquake"
        owners["/usr/bin/tyr-quake"] = "fplinux-tyrquake"
    if "fplinux-usb-gadget" in packages:
        owners.update(
            {
                "/usr/libexec/fplinux/usb-gadget": "fplinux-usb-gadget",
                "/etc/init.d/fplinux-usb-gadget": "fplinux-usb-gadget-openrc",
                "/etc/init.d/fplinux-usb-dhcp": "fplinux-usb-gadget-openrc",
            }
        )
    if "fplinux-ssh" in packages:
        owners.update(
            {
                "/usr/libexec/fplinux/ssh-server": "fplinux-ssh",
                "/usr/bin/fplinux-session-id": "fplinux-ssh",
                "/usr/bin/fplinux-clock": "fplinux-ssh",
                "/etc/init.d/fplinux-ssh": "fplinux-ssh-openrc",
            }
        )
    for path, package in owners.items():
        require_file(root / path.removeprefix("/"))
        _require_apk_owner(root, path, package)
    if "fplinux-bluetooth" in packages:
        for path in (
            "/usr/lib/bluetooth/bluetoothd",
            "/usr/lib/bluetooth/obexd",
            "/usr/bin/bluetoothctl",
            "/usr/share/dbus-1/system.d/bluetooth.conf",
            "/usr/share/dbus-1/system.d/obex.conf",
        ):
            _require_apk_owner(root, path, "fplinux-bluez")
        _require_apk_owner(root, "/usr/lib/libfplinux-bluez-glib-2.0.so.0", "fplinux-bluez-glib")
    if "fplinux-alsa-lib" in packages:
        _require_apk_owner(root, "/usr/lib/libasound.so.2", "fplinux-alsa-lib")
    if "fplinux-apk-tools" in packages:
        # The Mbed TLS package manager replaces the minirootfs apk-tools, and
        # the OpenSSL closure that only apk-tools needed must not remain.
        _require_apk_owner(root, "/sbin/apk", "fplinux-apk-tools")
        for package in ("apk-tools", "libapk", "libcrypto3", "libssl3"):
            if _alpine_package_installed(root, package):
                fail(f"replaced Alpine package remains in the rootfs: {package}")

    _require_openrc_service(root, "boot", "networking")
    _require_openrc_service(root, "default", "fplinux-brightness")
    _require_openrc_service(root, "default", "fplinux-terminal")
    if "fplinux-input" in packages:
        _require_openrc_service(root, "default", "fplinux-input")
    if "fplinux-usb-gadget" in packages:
        _require_openrc_service(root, "sysinit", "fplinux-usb-gadget")
        _require_openrc_service(root, "default", "fplinux-usb-dhcp")
    if "fplinux-ssh" in packages:
        _require_openrc_service(root, "default", "fplinux-ssh")
    if "fplinux-microsd-root" in packages:
        for service in ("killprocs", "savecache", "mount-ro"):
            _require_openrc_service(root, "shutdown", service)

    fstab = require_file(root / "etc/fstab").read_text(encoding="utf-8")
    if "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0" not in fstab:
        fail("Alpine fstab must mount /tmp as tmpfs")

    world = require_file(root / "etc/apk/world").read_text(encoding="utf-8").splitlines()
    if any("><Q" in entry for entry in world):
        fail("Alpine world must not contain checksum-pinned non-repository packages")
    selected_world = {entry for entry in world if entry.startswith("fplinux-")}
    if selected_world != set(packages):
        fail("Alpine world does not contain the exact selected FPLinux package set")
    for package in bundle_packages:
        _require_bundle_package_absent(root, package)
    _require_bundle_packages_installable(root, bundle_apks)

    inittab = require_file(root / "etc/inittab").read_text(encoding="utf-8")
    if "ttyGS" in inittab or "getty" in inittab:
        fail("Alpine inittab must leave all interactive consoles to OpenRC services")
    for obsolete in (
        "etc/init.d/fplinux-usb-getty",
        "etc/runlevels/default/fplinux-usb-getty",
    ):
        if (root / obsolete).exists() or (root / obsolete).is_symlink():
            fail(f"legacy USB ACM shell is present: /{obsolete}")
    for obsolete in (
        "etc/fplinux-build",
        "usr/libexec/fplinux/init",
        "usr/libexec/fplinux/usb-getty",
    ):
        if (root / obsolete).exists() or (root / obsolete).is_symlink():
            fail(f"obsolete pre-Alpine runtime path is present: /{obsolete}")
    firmware_inputs.verify_installed_firmware_inputs(root, firmware)
    brightness_path = root / "etc/fplinux/brightness.conf"
    if display_brightness is None:
        if brightness_path.exists() or brightness_path.is_symlink():
            fail("Alpine rootfs has brightness configuration without a target table")
    elif require_file(brightness_path).read_text(encoding="utf-8") != _brightness_config_text(
        display_brightness
    ):
        fail("Alpine rootfs brightness configuration does not match the target table")
