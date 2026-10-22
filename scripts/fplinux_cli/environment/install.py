# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the build environment from its verified local installation inputs."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Mapping


@dataclass(frozen=True)
class ToolInput:
    """An installation archive and the release or commit selected by the lock."""

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


def _extract_archive(
    archive: Path, destination: Path, *members: str, strip_components: int = 0
) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "tar",
            "--warning=no-unknown-keyword",
            "--extract",
            "--file",
            archive,
            "--directory",
            destination,
            f"--strip-components={strip_components}",
            *members,
        ],
        check=True,
    )


def _prepare_system(declarations: dict[str, Any]) -> None:
    repositories = dict.fromkeys(
        record["url"].rsplit("/", 2)[0]
        for record in declarations["input"]
        if record["purpose"] == "environment-package"
    )
    Path("/etc/apk/repositories").write_text("\n".join(repositories) + "\n", encoding="utf-8")
    selectors = [package for group in declarations["selection"]["apk_groups"] for package in group]
    subprocess.run(["apk", "add", "--no-network", "--no-cache", *selectors], check=True)
    subprocess.run(["adduser", "-D", "-u", "1000", "builder"], check=True)
    subprocess.run(["addgroup", "builder", "abuild"], check=True)


def _install_python(lock: Path, inputs: Path, resources: Path) -> None:
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
            "/opt/quality/python",
        ],
        check=True,
    )
    (Path(sysconfig.get_path("purelib")) / "fplinux-quality.pth").write_text(
        "import sys; sys.path.insert(0, '/opt/quality/python')\n", encoding="utf-8"
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
    subprocess.run(
        [
            sys.executable,
            "-B",
            "-c",
            (
                "import dtschema; import importlib.metadata; "
                'print(importlib.metadata.version("dtschema"))'
            ),
        ],
        check=True,
    )
    subprocess.run([sys.executable, "-B", "-m", "pytest", "--version"], check=True)


def _install_node(tools: Mapping[str, ToolInput], resources: Path, temporary: Path) -> None:
    node = tools["node"]
    npm = tools["npm"]
    node_root = Path("/opt/quality/node")
    _extract_archive(node.archive, node_root, strip_components=1)
    npm_root = node_root / "lib/node_modules/npm"
    shutil.rmtree(npm_root)
    _extract_archive(npm.archive, npm_root, strip_components=1)
    for executable, script in (("npm", "npm-cli.js"), ("npx", "npx-cli.js")):
        link = node_root / "bin" / executable
        link.unlink(missing_ok=True)
        link.symlink_to(f"../lib/node_modules/npm/bin/{script}")
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
    for archive in sorted((resources / "npm").glob("*.tgz")):
        subprocess.run(
            [
                "npm",
                "cache",
                "add",
                "--offline",
                "--ignore-scripts",
                "--cache",
                npm_cache,
                archive,
            ],
            check=True,
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
    _extract_archive(sparse.archive, sparse_root)
    sparse_source = sparse_root / f"sparse-{sparse.identity}"
    subprocess.run(["make", "-C", sparse_source, "sparse"], check=True)
    _install_file(sparse_source / "sparse", binaries / "sparse", mode=0o755)

    ruff_root = temporary / "ruff"
    _extract_archive(tools["ruff"].archive, ruff_root)
    _install_file(ruff_root / "ruff-x86_64-unknown-linux-musl/ruff", binaries / "ruff", mode=0o755)
    _extract_archive(tools["typos"].archive, binaries, "./typos")
    google = tools["vale-google"]
    _extract_archive(
        google.archive,
        Path("/opt/quality/vale-styles/Google"),
        f"Google-{google.identity}/Google",
        strip_components=2,
    )
    gitleaks_root = temporary / "gitleaks"
    _extract_archive(tools["gitleaks"].archive, gitleaks_root, "gitleaks")
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
    for command in ("sparse", "ruff", "reuse", "typos", "vale"):
        subprocess.run([command, "--version"], check=True)
    subprocess.run(["gitleaks", "version"], check=True)


def _install_gui_headers(tools: Mapping[str, ToolInput], temporary: Path) -> None:
    include = Path("/opt/quality/include")
    include.mkdir(parents=True, exist_ok=True)
    lvgl = tools["lvgl"]
    lvgl_root = temporary / "lvgl"
    _extract_archive(lvgl.archive, lvgl_root)
    lvgl_source = lvgl_root / f"lvgl-{lvgl.identity}"
    shutil.copytree(lvgl_source / "include/lvgl", include / "lvgl")
    _install_file(
        lvgl_source / "src/libs/lodepng/lodepng.h", include / "lvgl/src/libs/lodepng/lodepng.h"
    )
    _install_file(lvgl.archive, Path("/opt/quality/src/lvgl.tar.gz"))

    weston = tools["weston"]
    weston_root = temporary / "weston"
    _extract_archive(weston.archive, weston_root)
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
    _extract_archive(
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
    _extract_archive(tool.archive, source_root)
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


def _install_quality_sources(resources: Path, destination: Path) -> None:
    """Install the captured source inputs consumed by offline quality tools."""
    shutil.copytree(resources / "quality/spreadtrum-flash", destination / "spreadtrum-flash")
    for source in sorted((resources / "quality/ui-fonts").iterdir()):
        _install_file(source, destination / "ui-fonts" / source.name)


def _install_terminal_font(resources: Path, temporary: Path, destination: Path) -> None:
    """Build the offline renderer's bitmap fonts through their package producer."""
    source = temporary / "terminus"
    source.mkdir()
    for archive in sorted((resources / "quality/terminus").iterdir()):
        _extract_archive(archive, source)
    aport = resources / "alpine/aports/fplinux-font-terminus/APKBUILD"
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
    with args.lock.open("rb") as stream:
        declarations = tomllib.load(stream)
    tools = _tool_inputs(declarations, args.inputs)
    _prepare_system(declarations)
    _install_python(args.lock, args.inputs, args.resources)
    with tempfile.TemporaryDirectory(prefix="fplinux-environment-install-") as temporary:
        temporary_root = Path(temporary)
        _install_node(tools, args.resources, temporary_root)
        _install_cross_aliases()
        _install_quality_tools(tools, temporary_root)
        _install_gui_headers(tools, temporary_root)
        _install_libtsm(tools["libtsm"], args.resources, temporary_root)
        _install_terminal_font(args.resources, temporary_root, Path("/opt/quality/fonts"))
    _finish_environment(args.resources, args.recipe, args.generation)


if __name__ == "__main__":
    main()
