# SPDX-License-Identifier: GPL-2.0-only
"""Cross-build the Windows driver helper without executing Windows programs."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from string import Template
from textwrap import dedent
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from fplinux_cli.build.host_tools.windows_environment import WindowsBuildEnvironment

_PATCHES = tuple(
    Path(__file__).with_name(name) for name in ("source-date-epoch.patch", "inbox-winusb.patch")
)


def _configure_driver(source: Path, *, source_date_epoch: int) -> None:
    """Give the inbox driver package the helper's source version and fixed UTC date."""
    resource = (source / "libwdi/libwdi.rc").read_text(encoding="cp1252")
    version = re.search(r"^\s*FILEVERSION\s+(\d+),(\d+),(\d+),(\d+)\s*$", resource, re.MULTILINE)
    if version is None:
        message = "libwdi resource lacks its file version"
        raise ValueError(message)
    driver_version = ".".join(version.groups())
    driver_date = datetime.fromtimestamp(source_date_epoch, UTC).strftime("%m/%d/%Y")
    (source / "config.h").write_text(
        dedent("""\
            #define OPT_M64
            #define WINUSB_DRIVER_DATE "{driver_date}"
            #define WINUSB_DRIVER_VERSION "{driver_version}"
            #define ENABLE_LOGGING 1
            #define _GNU_SOURCE 1
            """).format(driver_date=driver_date, driver_version=driver_version),
        encoding="utf-8",
    )


def _build_driver(
    *,
    toolchain: Path,
    source: Path,
    output: Path,
    source_date_epoch: int,
    environment: WindowsBuildEnvironment | None = None,
) -> Path:
    """Compile the timestamp-adjusted native embedder and static x64 helper."""
    patches = [source / path.name for path in _PATCHES]
    for original, patch in zip(_PATCHES, patches, strict=True):
        shutil.copyfile(original, patch)
    if environment is None:
        for patch in patches:
            subprocess.run(["patch", "--fuzz=0", "-p1", "-i", str(patch)], cwd=source, check=True)
    else:
        environment.apply_patches(source, patches)
    _configure_driver(source, source_date_epoch=source_date_epoch)
    compiler = toolchain / "bin/x86_64-w64-mingw32-gcc"
    windres = toolchain / "bin/x86_64-w64-mingw32-windres"
    flags = [
        "-O2",
        "-g0",
        "-std=gnu99",
        "-DWINVER=0x0a00",
        "-D_WIN32_WINNT=0x0a00",
        "-D_WIN32_IE=0x0700",
        "-DCOBJMACROS",
        "-static",
        "-Wl,--no-insert-timestamp",
    ]
    library = source / "libwdi"
    subprocess.run(
        [
            str(compiler),
            *flags,
            "-o",
            "installer_x64.exe",
            "installer.c",
            "-lsetupapi",
            "-lnewdev",
            "-lole32",
        ],
        cwd=library,
        check=True,
    )
    compile_embedder = ["gcc", "-std=gnu99", "-I..", "embedder.c", "-o", "embedder"]
    run_embedder = [str(library / "embedder"), "embedded.h"]
    if environment is None:
        subprocess.run(compile_embedder, cwd=library, check=True)
        subprocess.run(
            run_embedder,
            cwd=library,
            env={**os.environ, "SOURCE_DATE_EPOCH": str(source_date_epoch)},
            check=True,
        )
    else:
        environment.run(compile_embedder, cwd=library)
        environment.run(
            run_embedder,
            cwd=library,
            variables={"SOURCE_DATE_EPOCH": str(source_date_epoch)},
        )
    for directory, name in ((library, "libwdi"), (source / "examples", "wdi-simple")):
        subprocess.run(
            [str(windres), "-i", f"{name}.rc", "-o", f"{name}_rc.o"],
            cwd=directory,
            check=True,
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(compiler),
            *flags,
            "-I..",
            "-I../libwdi",
            "-Igetopt",
            # Read the bundled declarations before the CRT's __argc/__argv macros.
            "-include",
            "getopt/getopt.h",
            "-DHAVE_STRING_H=1",
            "-o",
            str(output),
            "wdi-simple.c",
            "getopt/getopt.c",
            "getopt/getopt1.c",
            "../libwdi/logging.c",
            "../libwdi/tokenizer.c",
            "../libwdi/vid_data.c",
            "../libwdi/pki.c",
            "../libwdi/libwdi_dlg.c",
            "../libwdi/libwdi.c",
            "wdi-simple_rc.o",
            "../libwdi/libwdi_rc.o",
            "-lsetupapi",
            "-lole32",
            "-lntdll",
            "-lcomctl32",
            "-luuid",
        ],
        cwd=source / "examples",
        check=True,
    )
    return library / "installer_x64.exe"


def prepare_windows_driver(*, cache: Path, offline: bool) -> dict[str, Path]:
    """Prepare the helper and redistribution material under the caller's cache lock."""
    # Keep the delivered build recipe usable without the project Python package.
    from fplinux_cli import common  # noqa: PLC0415
    from fplinux_cli.build.environment import SOURCE_DATE_EPOCH  # noqa: PLC0415
    from fplinux_cli.build.host_tools.host_package import (  # noqa: PLC0415
        copy_mingw_licenses,
        publish_host_package,
    )
    from fplinux_cli.build.host_tools.windows import verify_windows_dependencies  # noqa: PLC0415
    from fplinux_cli.build.host_tools.windows_environment import (  # noqa: PLC0415
        WindowsBuildEnvironment,
    )
    from fplinux_cli.manifests.source_catalog import (  # noqa: PLC0415
        load_host_sources,
        resolve_source,
    )

    cache = cache.resolve()
    root = cache / "windows-driver"
    lock = load_host_sources(common.ROOT, "windows-driver.lock.toml")
    host_lock = load_host_sources(common.ROOT, "windows-host.lock.toml")
    toolchain_entry = host_lock["llvm_mingw"]
    archives = {
        name: resolve_source(common.ROOT, entry["source_id"]).acquire(cache, offline=offline)
        for name, entry in lock.items()
    }
    toolchain_archive = resolve_source(common.ROOT, toolchain_entry["source_id"]).acquire(
        cache, offline=offline
    )
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root, prefix=".build-") as temporary:
        work = Path(temporary)
        with tarfile.open(toolchain_archive) as archive:
            archive.extractall(work / "toolchain", filter="data")
        toolchain = work / "toolchain" / toolchain_entry["directory"]
        with tarfile.open(archives["libwdi"]) as archive:
            archive.extractall(work, filter="data")
        source = work / lock["libwdi"]["directory"]
        environment = WindowsBuildEnvironment(
            work=work,
            inputs=[*archives.values(), toolchain_archive],
        )
        package = work / "package"
        installer = _build_driver(
            toolchain=toolchain,
            source=source,
            output=package / "tools/driver/wdi-simple.exe",
            source_date_epoch=int(SOURCE_DATE_EPOCH),
            environment=environment,
        )
        verify_windows_dependencies(
            {"installer_x64.exe": installer},
            environment=environment,
        )
        licenses = package / "licenses/windows-driver"
        licenses.mkdir(parents=True)
        for name in ("COPYING", "COPYING-LGPL"):
            shutil.copyfile(source / name, licenses / f"libwdi-{name}.txt")
        copy_mingw_licenses(toolchain, licenses)
        sources = package / "sources/windows-driver"
        sources.mkdir(parents=True)
        shutil.copyfile(archives["libwdi"], sources / archives["libwdi"].name)
        shutil.copyfile(Path(__file__), sources / "build.py")
        for patch in _PATCHES:
            shutil.copyfile(patch, sources / patch.name)
        for name in ("windows-driver.lock.toml", "windows-host.lock.toml"):
            shutil.copyfile(common.ROOT / name, sources / name)
        notice = {
            name: {key: entry[key] for key in ("url", "sha256", "size")}
            for name, entry in {**lock, "llvm_mingw": toolchain_entry}.items()
        }
        (sources / "downloads.json").write_bytes(common.canonical_json_bytes(notice))
        readme_template = Template(
            Path(__file__).with_name("windows-driver-README.txt.in").read_text(encoding="utf-8")
        )
        (sources / "README.txt").write_text(
            readme_template.substitute(SOURCE_DATE_EPOCH=SOURCE_DATE_EPOCH), encoding="utf-8"
        )
        files = {
            path.relative_to(package).as_posix(): path
            for path in package.rglob("*")
            if path.is_file()
        }
        verify_windows_dependencies(files, environment=environment)
        return publish_host_package(package, root / "package", relative_paths=files)


def _main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-date-epoch", required=True, type=int)
    args = parser.parse_args()
    _build_driver(
        toolchain=args.toolchain.resolve(),
        source=args.source.resolve(),
        output=args.output.resolve(),
        source_date_epoch=args.source_date_epoch,
    )


if __name__ == "__main__":
    _main()
