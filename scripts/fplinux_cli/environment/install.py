# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the build environment from its verified local installation inputs."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from dataclasses import dataclass
from importlib.metadata import Distribution
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .archives import extract_archive
from .node_runtime import install_node_runtime, populate_npm_cache

if TYPE_CHECKING:
    from collections.abc import Mapping


@dataclass(frozen=True)
class ToolInput:
    """An installation input and the release or commit selected by the lock."""

    archive: Path
    identity: str


def _tool_inputs(declarations: dict[str, Any], inputs: Path) -> dict[str, ToolInput]:
    records = {record["key"]: record for record in declarations["input"]}
    tools: dict[str, ToolInput] = {}
    for name, metadata in declarations["tools"].items():
        record = records[metadata["input"]]
        identity = metadata["commit"] if name == "sparse" else metadata["version"]
        tools[name] = ToolInput(inputs / record["destination"], identity)
    return tools


def _install_file(source: Path, destination: Path, *, mode: int = 0o644) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)


def _prepare_system(declarations: dict[str, Any]) -> None:
    repositories = dict.fromkeys(
        record["url"].rsplit("/", 2)[0]
        for record in declarations["input"]
        if record["purpose"] == "environment-package"
    )
    Path("/etc/apk/repositories").write_text("\n".join(repositories) + "\n", encoding="utf-8")
    selectors = [package for group in declarations["selection"]["apk_groups"] for package in group]
    subprocess.run(["apk", "add", "--no-network", "--no-cache", *selectors], check=True)
    subprocess.run(
        [sys.executable, "-B", "-c", "from clang import cindex; cindex.Index.create()"],
        check=True,
    )
    subprocess.run(["adduser", "-D", "-u", "1000", "builder"], check=True)
    subprocess.run(["addgroup", "builder", "abuild"], check=True)


def _install_python(lock: Path, inputs: Path, resources: Path) -> None:
    packages = Path("/opt/quality/python")
    subprocess.run(
        [
            sys.executable,
            "-B",
            resources / "scripts/fplinux_cli/environment/install_wheels.py",
            "--lock",
            lock,
            "--inputs",
            inputs,
            "--destination",
            packages,
        ],
        check=True,
    )
    (Path(sysconfig.get_path("purelib")) / "fplinux-quality.pth").write_text(
        f"import sys; sys.path.insert(0, {str(packages)!r})\n", encoding="utf-8"
    )
    subprocess.run(
        [
            sys.executable,
            "-B",
            "-c",
            (
                "import tomlkit; import ruamel.yaml; "
                "print(tomlkit.__version__); print(ruamel.yaml.__version__)"
            ),
        ],
        check=True,
    )
    _install_dtschema_commands(packages)
    for module in ("pytest", "mypy", "reuse"):
        subprocess.run([sys.executable, "-B", "-m", module, "--version"], check=True)


def _install_dtschema_commands(packages: Path) -> None:
    distributions = Distribution.discover(name="dtschema", path=[str(packages)])
    schema = next(iter(distributions), None)
    if schema is None:
        message = "installed Python wheels do not contain dtschema"
        raise SystemExit(message)
    binaries = Path("/opt/quality/bin")
    binaries.mkdir(parents=True, exist_ok=True)
    for entry in schema.entry_points.select(group="console_scripts"):
        command = binaries / entry.name
        command.write_text(
            f"#!{sys.executable}\n"
            f"from {entry.module} import {entry.attr}\n"
            f"raise SystemExit({entry.attr}())\n",
            encoding="utf-8",
        )
        command.chmod(0o755)
    subprocess.run([binaries / "dt-validate", "--version"], check=True)


def _install_dtc(tool: ToolInput, temporary: Path) -> None:
    source = temporary / "dtc"
    build = temporary / "dtc-build"
    extract_archive(tool.archive, source, strip_components=1)
    subprocess.run(
        [
            "meson",
            "setup",
            build,
            source,
            "--wrap-mode=nodownload",
            "--prefix=/usr",
            "--libdir=lib",
            "--buildtype=release",
            "-Ddefault_library=shared",
            "-Dpython=enabled",
            "-Dpython.bytecompile=-1",
            "-Dtests=false",
            "-Dvalgrind=disabled",
            "-Dyaml=disabled",
        ],
        check=True,
    )
    subprocess.run(["meson", "compile", "-C", build], check=True)
    # Older U-Boot host tools need their own libfdt headers.
    subprocess.run(
        ["meson", "install", "-C", build, "--no-rebuild", "--tags", "runtime,python-runtime"],
        check=True,
    )
    _install_file(build / "pylibfdt/libfdt.py", Path(sysconfig.get_path("platlib")) / "libfdt.py")
    version = subprocess.check_output(["dtc", "--version"], text=True).strip()
    if version != f"Version: DTC {tool.identity}":
        message = "installed dtc version does not match the environment lock"
        raise SystemExit(message)


def _install_node(tools: Mapping[str, ToolInput], resources: Path, temporary: Path) -> None:
    node = tools["node"]
    npm = tools["npm"]
    install_node_runtime(node.archive, npm.archive, Path("/opt/quality/node"))
    npm_cache = temporary / "npm-cache"
    actual_node = subprocess.check_output(["node", "--version"], text=True).strip()
    actual_npm = subprocess.check_output(
        ["npm", "--offline", "--cache", npm_cache, "--version"], text=True
    ).strip()
    if actual_node != f"v{node.identity}" or actual_npm != npm.identity:
        message = "installed Node.js or npm version does not match the environment lock"
        raise SystemExit(message)

    node_tools = Path("/opt/quality/node-tools")
    for filename in ("package.json", "package-lock.json"):
        _install_file(resources / filename, node_tools / filename)
    populate_npm_cache(
        Path("/opt/quality/node/bin/npm"), npm_cache, sorted((resources / "npm").glob("*.tgz"))
    )
    subprocess.run(
        [
            "npm",
            "ci",
            "--offline",
            "--ignore-scripts",
            "--cache",
            npm_cache,
            "--prefix",
            node_tools,
        ],
        check=True,
    )
    for command in ("commitlint", "prettier", "markdownlint-cli2"):
        subprocess.run([command, "--version"], check=True)


def _install_cross_aliases() -> None:
    directory = Path("/usr/local/bin")
    directory.mkdir(parents=True, exist_ok=True)
    executables = {"cc": "clang", "c++": "clang++", "ld": "ld.lld"}
    for name in (
        "ar",
        "nm",
        "objcopy",
        "objdump",
        "ranlib",
        "readelf",
        "size",
        "strings",
        "strip",
    ):
        executables[name] = f"arm-none-eabi-{name}"
    for name, executable in executables.items():
        (directory / f"armv7-alpine-linux-musleabihf-{name}").symlink_to(f"/usr/bin/{executable}")
    machine = subprocess.check_output(
        ["armv7-alpine-linux-musleabihf-cc", "-dumpmachine"], text=True
    ).strip()
    if machine != "armv7-alpine-linux-musleabihf":
        message = "the ARM compiler alias does not select the required target"
        raise SystemExit(message)
    subprocess.run(["arm-none-eabi-gcc", "--version"], check=True)


def _install_quality_tools(tools: Mapping[str, ToolInput], temporary: Path) -> None:
    binaries = Path("/opt/quality/bin")
    binaries.mkdir(parents=True, exist_ok=True)
    sparse = tools["sparse"]
    sparse_root = temporary / "sparse"
    extract_archive(sparse.archive, sparse_root)
    sparse_source = sparse_root / f"sparse-{sparse.identity}"
    subprocess.run(["make", "-C", sparse_source, "sparse"], check=True)
    _install_file(sparse_source / "sparse", binaries / "sparse", mode=0o755)

    ruff_root = temporary / "ruff"
    extract_archive(tools["ruff"].archive, ruff_root)
    _install_file(ruff_root / "ruff-x86_64-unknown-linux-musl/ruff", binaries / "ruff", mode=0o755)
    _install_file(tools["shfmt"].archive, binaries / "shfmt", mode=0o755)
    extract_archive(tools["typos"].archive, binaries, "./typos")
    extract_archive(tools["vale"].archive, binaries, "vale")
    google = tools["vale-google"]
    extract_archive(
        google.archive,
        Path("/opt/quality/vale-styles/Google"),
        f"Google-{google.identity}/Google",
        strip_components=2,
    )
    gitleaks_root = temporary / "gitleaks"
    extract_archive(tools["gitleaks"].archive, gitleaks_root, "gitleaks")
    _install_file(gitleaks_root / "gitleaks", binaries / "gitleaks", mode=0o755)
    selftest = temporary / "gitleaks-selftest"
    selftest.mkdir()
    token = "ghp_" + "7cE9vKwP3qN8sXbL5rM2tDfA" + "6zH4uYjR1gV0"
    (selftest / "secret.txt").write_text(f'token = "{token}"\n', encoding="utf-8")
    status = subprocess.run(
        [
            "gitleaks",
            "detect",
            "--no-banner",
            "--no-git",
            "--source",
            selftest,
            "--exit-code",
            "1",
        ],
        check=False,
    ).returncode
    if status != 1:
        raise SystemExit(f"gitleaks detection self-test returned {status}, expected 1")
    for command in ("sparse", "ruff", "shfmt", "typos", "vale"):
        subprocess.run([command, "--version"], check=True)
    subprocess.run(["gitleaks", "version"], check=True)


def _install_gui_headers(tools: Mapping[str, ToolInput], temporary: Path) -> None:
    include = Path("/opt/quality/include")
    include.mkdir(parents=True, exist_ok=True)
    lvgl = tools["lvgl"]
    lvgl_root = temporary / "lvgl"
    extract_archive(lvgl.archive, lvgl_root)
    lvgl_source = lvgl_root / f"lvgl-{lvgl.identity}"
    shutil.copytree(lvgl_source / "include/lvgl", include / "lvgl")
    _install_file(
        lvgl_source / "src/libs/lodepng/lodepng.h", include / "lvgl/src/libs/lodepng/lodepng.h"
    )
    _install_file(lvgl.archive, Path("/opt/quality/src/lvgl.tar.gz"))

    weston = tools["weston"]
    weston_root = temporary / "weston"
    extract_archive(weston.archive, weston_root)
    weston_source = weston_root / f"weston-{weston.identity}"
    major, minor, micro = weston.identity.split(".")
    weston_headers = include / f"libweston-{major}/libweston"
    for header in sorted((weston_source / "include/libweston").glob("*.h")):
        _install_file(header, weston_headers / header.name)
    version_header = (weston_source / "include/libweston/version.h.in").read_text(encoding="utf-8")
    for name, value in (
        ("WESTON_VERSION_MAJOR", major),
        ("WESTON_VERSION_MINOR", minor),
        ("WESTON_VERSION_MICRO", micro),
        ("WESTON_VERSION", weston.identity),
    ):
        version_header = version_header.replace(f"@{name}@", value)
    (weston_headers / "version.h").write_text(version_header, encoding="utf-8")
    _install_file(weston_source / "frontend/weston.h", include / "weston/weston.h")
    extract_archive(
        tools["mesa-headers"].archive,
        include,
        "usr/include/EGL",
        "usr/include/GL",
        "usr/include/GLES2",
        "usr/include/GLES3",
        "usr/include/KHR",
        strip_components=2,
    )


def _install_libtsm(tool: ToolInput, resources: Path, temporary: Path) -> None:
    source_root = temporary / "libtsm"
    extract_archive(tool.archive, source_root)
    source = source_root / f"libtsm-{tool.identity}"
    subprocess.run(
        [
            "patch",
            "--directory",
            source,
            "-p1",
            "--input",
            resources / "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
        ],
        check=True,
    )
    library = source_root / "libtsm.so.4"
    source_files = (
        "src/tsm/tsm-render.c",
        "src/tsm/tsm-screen.c",
        "src/tsm/tsm-selection.c",
        "src/tsm/tsm-unicode.c",
        "src/tsm/tsm-vte-charsets.c",
        "src/tsm/tsm-vte.c",
        "src/shared/shl-htable.c",
        "external/wcwidth/wcwidth.c",
    )
    subprocess.run(
        [
            "cc",
            "-Os",
            "-std=gnu99",
            "-D_GNU_SOURCE",
            "-fPIC",
            "-shared",
            "-Wl,-soname,libtsm.so.4",
            f"-Wl,--version-script={source}/src/tsm/libtsm.sym",
            f"-I{source}/src/tsm",
            f"-I{source}/src/shared",
            f"-I{source}/external",
            f"-I{source}/external/wcwidth",
            *(source / filename for filename in source_files),
            "-o",
            library,
        ],
        check=True,
    )
    _install_file(library, Path("/usr/lib/libtsm.so.4"), mode=0o755)
    Path("/usr/lib/libtsm.so").symlink_to("libtsm.so.4")
    _install_file(source / "src/tsm/libtsm.h", Path("/usr/include/libtsm.h"))
    pkgconfig = Path("/usr/lib/pkgconfig/libtsm.pc")
    pkgconfig.parent.mkdir(parents=True, exist_ok=True)
    pkgconfig.write_text(
        "\n".join(
            (
                "prefix=/usr",
                "libdir=/usr/lib",
                "includedir=/usr/include",
                "Name: libtsm",
                "Description: Terminal state machine",
                f"Version: {tool.identity}",
                "Libs: -L/usr/lib -ltsm",
                "Cflags: -I/usr/include",
                "",
            )
        ),
        encoding="utf-8",
    )
    subprocess.run(["pkg-config", "--modversion", "libtsm"], check=True)


def _install_mpg123_headers(tool: ToolInput, temporary: Path) -> None:
    """Prepare matching decoder headers and the original used by host tests."""
    source_root = temporary / "mpg123"
    extract_archive(tool.archive, source_root)
    source = source_root / f"mpg123-{tool.identity}"
    subprocess.run(
        [
            "./configure",
            "--disable-components",
            "--enable-libmpg123",
            "--disable-network",
            "--with-audio=dummy",
            "--with-cpu=generic",
        ],
        cwd=source,
        check=True,
    )
    _install_file(source / "src/include/mpg123.h", Path("/opt/quality/include/mpg123.h"))
    _install_file(source / "src/include/fmt123.h", Path("/opt/quality/include/fmt123.h"))
    _install_file(tool.archive, Path("/opt/quality/src/mpg123.tar.bz2"))


def _install_ffmpeg_amr_nb(tool: ToolInput, resources: Path, temporary: Path) -> None:
    """Prepare the pinned native FFmpeg decoders for host tests and analysis."""
    source = temporary / "ffmpeg"
    extract_archive(tool.archive, source, strip_components=1)
    configure = resources / "alpine/aports/fplinux-amr-nb/configure-amr-nb.sh"
    subprocess.run(["sh", configure, "--disable-x86asm"], cwd=source, check=True)
    subprocess.run(["make"], cwd=source, check=True)
    subprocess.run(["make", "install-libs", "install-headers"], cwd=source, check=True)


def _install_tinysoundfont_headers(
    tool: ToolInput,
    resources: Path,
    temporary: Path,
) -> None:
    """Prepare the same bounded renderer header used by the target package."""
    source = temporary / "tinysoundfont"
    source.mkdir()
    header = source / "tsf.h"
    header.write_text(tool.archive.read_text(encoding="utf-8"), encoding="utf-8")
    subprocess.run(
        [
            "patch",
            "-p1",
            "--input",
            resources / "alpine/aports/fplinux-midi/borrowed-s16.patch",
        ],
        cwd=source,
        check=True,
    )
    _install_file(header, Path("/opt/quality/include/tsf.h"))
    _install_file(tool.archive, Path("/opt/quality/src/tsf.h"))


def _install_quality_sources(resources: Path, destination: Path) -> None:
    """Install the captured source inputs consumed by offline quality tools."""
    shutil.copytree(resources / "quality/spreadtrum-flash", destination / "spreadtrum-flash")
    for name in ("ui-fonts", "ui-icons"):
        for source in sorted((resources / "quality" / name).iterdir()):
            _install_file(source, destination / name / source.name)


def _install_terminal_font(resources: Path, temporary: Path, destination: Path) -> None:
    """Build the offline renderer's bitmap fonts through their package producer."""
    source = temporary / "terminus"
    source.mkdir()
    for archive in sorted((resources / "quality/terminus").iterdir()):
        extract_archive(archive, source)
    aport = resources / "quality/terminus-aport/APKBUILD"
    subprocess.run(
        [
            "sh",
            "-eu",
            "-c",
            (
                'srcdir=$1; . "$2"; cd "$builddir"; build; '
                'install -Dm644 ter-u12n-terminal.psf "$3/ter-u12n-terminal.psf"; '
                'install -Dm644 ter-u16n-terminal.psf "$3/ter-u16n-terminal.psf"; '
                'install -Dm644 OFL.TXT "$3/Terminus-OFL.txt"'
            ),
            "sh",
            source,
            aport,
            destination,
        ],
        check=True,
    )


def _finish_environment(resources: Path, recipe: str, generation: str) -> None:
    _install_quality_sources(resources, Path("/opt/quality/src"))
    Path("/var/log/apk.log").unlink(missing_ok=True)
    for directory in (
        "/cache/analysis",
        "/cache/ccache",
        "/cache/downloads",
        "/cache/host-tools",
        "/cache/linux",
        "/cache/rootfs",
        "/tmp/fplinux-home",  # noqa: S108 -- directory belongs to the prepared sandbox rootfs.
        "/workspace",
        "/work",
    ):
        Path(directory).mkdir(parents=True, exist_ok=True)
    for directory in ("/cache", "/tmp/fplinux-home", "/work"):  # noqa: S108 -- sandbox rootfs.
        Path(directory).chmod(0o1777)
    collector = Path("/usr/local/libexec/fplinux-environment-content.py")
    _install_file(resources / "scripts/fplinux_cli/environment/image_content.py", collector)
    content = subprocess.check_output([sys.executable, "-B", collector], text=True).strip()
    Path("/etc/fplinux-environment-state").write_text(
        f"{recipe}\n{generation}\n{content}\n", encoding="utf-8"
    )


def main() -> None:
    """Install the complete environment inside a fresh candidate rootfs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--recipe", required=True)
    parser.add_argument("--generation", required=True)
    args = parser.parse_args()
    declarations = json.loads(args.lock.read_text())
    tools = _tool_inputs(declarations, args.inputs)
    _prepare_system(declarations)
    with tempfile.TemporaryDirectory(prefix="fplinux-environment-install-") as temporary:
        temporary_root = Path(temporary)
        _install_dtc(tools["dtc"], temporary_root)
        _install_python(args.lock, args.inputs, args.resources)
        _install_node(tools, args.resources, temporary_root)
        _install_cross_aliases()
        _install_quality_tools(tools, temporary_root)
        _install_file(tools["fpdoom-host"].archive, Path("/opt/quality/src/fpdoom-host.tar.gz"))
        _install_file(
            tools["fpdoom-bootstrap"].archive, Path("/opt/quality/src/fpdoom-bootstrap.tar.gz")
        )
        _install_gui_headers(tools, temporary_root)
        _install_libtsm(tools["libtsm"], args.resources, temporary_root)
        _install_mpg123_headers(tools["mpg123"], temporary_root)
        _install_ffmpeg_amr_nb(tools["ffmpeg-amr-nb"], args.resources, temporary_root)
        _install_tinysoundfont_headers(tools["tinysoundfont"], args.resources, temporary_root)
        for tool, filename in (("soundfont", "soundfont.tar.gz"),):
            _install_file(tools[tool].archive, Path("/opt/quality/src") / filename)
        _install_terminal_font(args.resources, temporary_root, Path("/opt/quality/fonts"))
    _finish_environment(args.resources, args.recipe, args.generation)


if __name__ == "__main__":
    main()
