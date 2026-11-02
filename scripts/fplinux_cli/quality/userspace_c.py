# SPDX-License-Identifier: GPL-2.0-only
"""Select and analyze userspace C sources."""

from __future__ import annotations

import os
import shlex
import tempfile
from pathlib import Path, PurePosixPath

from fplinux_cli.alpine import registration
from fplinux_cli.common import ROOT, fail
from fplinux_cli.manifests.paths import discover_targets
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.quality import source_commands
from fplinux_cli.quality.scopes import is_project_c_source

APORT_ROOT = ("alpine", "aports")
PACKAGE_EMBEDDED_C_MARKER = "fplinux-check: package-embedded"
APORT_C_SUFFIXES = frozenset({".c"})
_PACKAGE_CONFIG_TIMEOUT = 30
_QUALITY_INCLUDE = "/opt/quality/include"
_LVGL_APORTS = frozenset(
    {
        "fplinux-calculator",
        "fplinux-clocks",
        "fplinux-example-apidemo",
        "fplinux-files",
        "fplinux-launcher",
        "fplinux-mre",
        "fplinux-phoneme",
        "fplinux-settings",
        "fplinux-snake",
        "fplinux-notes",
        "fplinux-terminal",
        "fplinux-ui",
    }
)
_UI_RUNTIME_SOURCE = "lib/fplinux/fplinux-ui.c"
_LVGL_RUNTIME_SOURCES = frozenset(
    {
        _UI_RUNTIME_SOURCE,
        "lib/fplinux/fplinux-ui-file-picker.c",
        "lib/fplinux/fplinux-ui-frame.c",
        "lib/fplinux/fplinux-ui-image.c",
        "lib/fplinux/fplinux-ui-listview.c",
        "lib/fplinux/fplinux-ui-menu.c",
        "lib/fplinux/fplinux-ui-screen.c",
        "lib/fplinux/fplinux-ui-style.c",
        "lib/fplinux/fplinux-ui-symbol-picker.c",
        "lib/fplinux/fplinux-ui-text.c",
        "lib/fplinux/fplinux-ui-view-settings.c",
    }
)


def package_c_is_embedded(path: Path) -> bool:
    """Return whether a package C file is compiled only inside an upstream tree."""
    with path.open(encoding="utf-8") as stream:
        return any(PACKAGE_EMBEDDED_C_MARKER in stream.readline() for _ in range(4))


def is_aport_source(path: Path, suffixes: frozenset[str]) -> bool:
    """Return whether a source belongs to any first-party Alpine aport."""
    relative = path.relative_to(ROOT)
    return relative.parts[:2] == APORT_ROOT and relative.suffix in suffixes


def is_shared_aport_source(path: Path, suffixes: frozenset[str]) -> bool:
    """Return whether a source is one canonical C/H file shared by aports."""
    relative = path.relative_to(ROOT)
    return (
        relative.as_posix() in registration.SHARED_APORT_SOURCE_PATHS
        and relative.suffix in suffixes
    )


def userspace_c_sources(
    files: list[Path], *, include_embedded: bool = False
) -> list[tuple[str, bool]]:
    """Discover userspace C and whether each source needs libusb."""
    result: dict[str, bool] = {}
    for path in files:
        if (
            is_aport_source(path, APORT_C_SUFFIXES)
            or is_shared_aport_source(path, APORT_C_SUFFIXES)
        ) and (include_embedded or not package_c_is_embedded(path)):
            result[path.relative_to(ROOT).as_posix()] = False

    platform_names = {load_target(target)["platform"] for target in discover_targets()}
    for platform_name in sorted(platform_names):
        platform = load_platform(platform_name)
        for recipe in platform["host"]["tools"]:
            if recipe["type"] != "cc-libusb":
                continue
            source = recipe["source"]
            path = ROOT / source
            if path.suffix != ".c" or path.is_symlink() or not path.is_file():
                fail(f"cc-libusb source must be a regular C file: {source}")
            result[source] = True

    if not result:
        fail("no userspace C sources were discovered")
    return sorted(result.items())


def project_c_format_sources(files: list[Path]) -> list[str]:
    """Discover formatted C/H outside the separately checked Linux tree."""
    sources = {
        path.relative_to(ROOT).as_posix()
        for path in files
        if is_project_c_source(path.relative_to(ROOT))
    }
    sources.update(
        source for source, _requires_libusb in userspace_c_sources(files, include_embedded=True)
    )
    return sorted(sources)


def pkg_config_cflags(package: str) -> list[str]:
    """Resolve the installed header flags for one declared C dependency."""
    result = source_commands.capture(
        ["pkg-config", "--cflags", package],
        timeout=_PACKAGE_CONFIG_TIMEOUT,
    )
    if result.returncode:
        fail(result.stderr.strip() or f"pkg-config could not resolve {package}")
    return shlex.split(result.stdout)


def userspace_c_include_flags(source: str) -> list[str]:
    """Return compile flags needed by one source's headers."""
    path = PurePosixPath(source)
    flags = ["-I", "include"]
    aport = path.parts[2] if path.parts[:2] == APORT_ROOT and len(path.parts) >= 3 else None
    if aport in _LVGL_APORTS or source in _LVGL_RUNTIME_SOURCES:
        flags.extend(
            (
                "-isystem",
                _QUALITY_INCLUDE,
                f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            )
        )
    if source == _UI_RUNTIME_SOURCE:
        flags.extend(("-I", "alpine/aports/fplinux-ui"))
        flags.extend(pkg_config_cflags("wayland-client"))
        flags.extend(pkg_config_cflags("xkbcommon"))
    if source == "lib/fplinux/fplinux-ui-session.c":
        flags.extend(pkg_config_cflags("json-c"))
    if source == "alpine/aports/fplinux-session/fplinux-shell.c":
        flags.extend(
            ("-isystem", _QUALITY_INCLUDE, "-isystem", f"{_QUALITY_INCLUDE}/libweston-16")
        )
        flags.extend(pkg_config_cflags("pixman-1"))
        flags.extend(pkg_config_cflags("wayland-server"))
        flags.extend(pkg_config_cflags("xkbcommon"))
        flags.extend(pkg_config_cflags("libinput"))
    if (
        path.parts[:3] == (*APORT_ROOT, "fplinux-bluetooth")
        or source in registration.BLUEZ_ADAPTER_SOURCES
    ):
        flags.extend(pkg_config_cflags("dbus-1"))
    if source == "lib/fplinux-shared/fplinux-input-session.c":
        flags.extend(pkg_config_cflags("libevdev"))
    shared = registration.SHARED_APORT_SOURCES.get(path.parent.name, ())
    uses_gpu = source in registration.GPU_OUTPUT_SOURCES or (
        "include/fplinux/fplinux-gpu.h" in shared
    )
    if uses_gpu:
        flags.extend(("-isystem", _QUALITY_INCLUDE))
    if (
        source in registration.DRM_SESSION_SOURCES
        or uses_gpu
        or ("include/fplinux/fplinux-drm-session.h" in shared)
    ):
        flags.extend(pkg_config_cflags("libdrm"))
    if (
        len(path.parts) >= 3
        and path.parts[:2] == APORT_ROOT
        and path.parts[2] in registration.SHARED_APORT_SOURCES
    ):
        directories = sorted(
            {
                str(PurePosixPath(shared).parent)
                for shared in registration.SHARED_APORT_SOURCES[path.parts[2]]
            }
        )
        flags.extend(flag for directory in directories for flag in ("-I", directory))
    elif path.parts[:2] in (("lib", "fplinux"), ("lib", "fplinux-shared")) or source == (
        "platforms/ums9117/host/fplinux-usb-keyboard.c"
    ):
        flags.extend(("-I", "include/fplinux"))
    return flags


def generate_ui_protocol_header(output: Path) -> None:
    """Generate the public xdg-shell client header used by the UI aport."""
    result = source_commands.capture(
        ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"],
        timeout=_PACKAGE_CONFIG_TIMEOUT,
    )
    if result.returncode or not result.stdout.strip():
        fail(result.stderr.strip() or "pkg-config could not resolve wayland-protocols")
    protocol = Path(result.stdout.strip()) / "stable/xdg-shell/xdg-shell.xml"
    source_commands.run(["wayland-scanner", "client-header", str(protocol), str(output)])


def run_userspace_analysis(output: Path, sources: list[tuple[str, bool]]) -> None:
    """Run Clang Static Analyzer for every discovered userspace C source."""
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        fail(f"invalid scan-build output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    if any(source == _UI_RUNTIME_SOURCE for source, _libusb in sources):
        generate_ui_protocol_header(output / "xdg-shell-client-protocol.h")
    activity = "lib/fplinux-shared/fplinux-activity.c"
    activity_consumers: tuple[tuple[str, str, tuple[str, ...]], ...] = (
        (activity, "fplinux-activity-masks.h", ()),
        ("lib/fplinux/fplinux-ui-screen.c", "fplinux-ui-activity.h", ("--lvgl",)),
    )
    for consumer, header, options in activity_consumers:
        if not any(source == consumer for source, _libusb in sources):
            continue
        source_commands.run(
            [
                "python3",
                "scripts/fplinux_activity.py",
                "--tokens",
                "lib/fplinux/fplinux-ui-profile.json",
                "--output",
                str(output / header),
                *options,
            ]
        )
    common_flags = [
        "-std=c11",
        "-O2",
        "-g0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fno-ident",
        "-c",
    ]
    needs_libusb = any(libusb for _source, libusb in sources)
    libusb_flags: list[str] = []
    if needs_libusb:
        libusb_flags = pkg_config_cflags("libusb-1.0")

    analyzer = [
        "scan-build",
        "--status-bugs",
        "--use-cc=clang",
        "-o",
        str(output / "reports"),
        "clang",
    ]
    for index, (source, requires_libusb) in enumerate(sources):
        flags = [*common_flags, *userspace_c_include_flags(source)]
        if source in {_UI_RUNTIME_SOURCE, activity, "lib/fplinux/fplinux-ui-screen.c"}:
            flags.extend(("-I", str(output)))
        if requires_libusb:
            flags.extend(libusb_flags)
            flags.append("-pthread")
        source_commands.run(
            [
                *analyzer,
                *flags,
                source,
                "-o",
                str(output / f"{index:02d}-{Path(source).stem}.o"),
            ]
        )


def check_userspace_c(sources: list[tuple[str, bool]]) -> None:
    """Select persistent check output when the container provides it."""
    persistent = os.environ.get("FPLINUX_SCAN_BUILD_DIR")
    if persistent is not None:
        output = Path(persistent)
        if not output.is_absolute():
            fail("FPLINUX_SCAN_BUILD_DIR must be absolute")
        run_userspace_analysis(output, sources)
        return
    with tempfile.TemporaryDirectory(prefix="fplinux-scan-build-") as temporary:
        run_userspace_analysis(Path(temporary), sources)
