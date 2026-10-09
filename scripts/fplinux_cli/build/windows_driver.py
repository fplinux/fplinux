# SPDX-License-Identifier: GPL-2.0-only
"""Cross-build the Windows driver helper without executing Windows programs."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from fplinux_cli.build.windows_environment import WindowsBuildEnvironment

_EMBEDDER_TIMESTAMP_PATCH = (
    """--- a/libwdi/embedder.c
+++ b/libwdi/embedder.c
@@ -410,7 +410,9 @@
 \t\t} else {
 \t\t\tprintf("\\n");
 \t\t}
-\t\tfile_time[i] = (int64_t)stbuf.st_ctime;
+\t\tfile_time[i] = getenv("SOURCE_DATE_EPOCH") != NULL
+\t\t\t? strtoll(getenv("SOURCE_DATE_EPOCH"), NULL, 10)
+\t\t\t: (int64_t)stbuf.st_ctime;
 \t\tfile_size[i] = (size_t)stbuf.st_size;
"""
    " \n \t\tbuffer = (unsigned char*) malloc(file_size[i]);\n"
)


def _extract_redist(
    archive: Path,
    source: Path,
    *,
    environment: WindowsBuildEnvironment | None = None,
) -> Path:
    """Extract the licensed x64 coinstallers without running the MSI installer."""
    extracted = source / "wdf-redistributables"
    command = ["7z", "x", "-y", f"-o{extracted}", str(archive)]
    if environment is None:
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    else:
        environment.run(command, cwd=source)
    destination = source / "redist/wdf/x64"
    destination.mkdir(parents=True, exist_ok=True)
    redistributable_list = (extracted / "redist.txt.x64").read_text(encoding="utf-8")
    for archived, name in (
        ("_WdfCoInstaller01011.dll_00001", "WdfCoInstaller01011.dll"),
        ("_winusbcoinstaller2.dll_00001", "winusbcoinstaller2.dll"),
    ):
        if f"redist\\wdf\\x64\\{name}".lower() not in redistributable_list.lower():
            raise ValueError(f"Windows driver resource is not listed for redistribution: {name}")
        shutil.copyfile(extracted / archived, destination / name)
    return extracted


def _build_driver(
    *,
    toolchain: Path,
    source: Path,
    output: Path,
    source_date_epoch: int,
    environment: WindowsBuildEnvironment | None = None,
) -> Path:
    """Compile the timestamp-adjusted native embedder and static x64 helper."""
    patch = source / "source-date-epoch.patch"
    patch.write_text(_EMBEDDER_TIMESTAMP_PATCH, encoding="utf-8")
    if environment is None:
        subprocess.run(["patch", "--fuzz=0", "-p1", "-i", str(patch)], cwd=source, check=True)
    else:
        environment.apply_patches(source, [patch])
    compiler = toolchain / "bin/x86_64-w64-mingw32-gcc"
    windres = toolchain / "bin/x86_64-w64-mingw32-windres"
    flags = [
        "-O2",
        "-g0",
        "-std=gnu99",
        "-DWINVER=0x0601",
        "-D_WIN32_WINNT=0x0601",
        "-D_WIN32_IE=0x0700",
        "-DCOBJMACROS",
        "-static",
        "-Wl,--no-insert-timestamp",
    ]
    (source / "config.h").write_text(
        '#define OPT_M64\n#define WDF_VER 1011\n#define COINSTALLER_DIR "wdf"\n'
        '#define X64_DIR "x64"\n#define WDK_DIR ".."\n'
        "#define ENABLE_LOGGING 1\n#define _GNU_SOURCE 1\n",
        encoding="utf-8",
    )
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
    from fplinux_cli.build.windows import verify_windows_dependencies  # noqa: PLC0415
    from fplinux_cli.build.windows_environment import WindowsBuildEnvironment  # noqa: PLC0415
    from fplinux_cli.environment.downloads import download_locked_file  # noqa: PLC0415

    cache = cache.resolve()
    root = cache / "windows-driver"
    lock = common.load_toml(common.ROOT / "windows-driver.lock.toml")
    host_lock = common.load_toml(common.ROOT / "windows-host.lock.toml")
    toolchain_entry = host_lock["llvm_mingw"]
    downloads = root / "downloads"
    archives = {
        name: download_locked_file(
            entry["url"],
            entry["sha256"],
            downloads / entry["name"],
            offline=offline,
            size=entry["size"],
        )
        for name, entry in lock.items()
    }
    toolchain_download = downloads / toolchain_entry["name"]
    existing_toolchain = cache / "windows-host/downloads" / toolchain_entry["name"]
    if (
        not toolchain_download.exists()
        and existing_toolchain.is_file()
        and existing_toolchain.stat().st_size == toolchain_entry["size"]
        and common.sha256_file(existing_toolchain) == toolchain_entry["sha256"]
    ):
        shutil.copyfile(existing_toolchain, toolchain_download)
    toolchain_archive = download_locked_file(
        toolchain_entry["url"],
        toolchain_entry["sha256"],
        toolchain_download,
        offline=offline,
        size=toolchain_entry["size"],
    )
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
        redist = _extract_redist(archives["wdf"], source, environment=environment)
        package = work / "package"
        installer = _build_driver(
            toolchain=toolchain,
            source=source,
            output=package / "tools/driver/wdi-simple.exe",
            source_date_epoch=int(SOURCE_DATE_EPOCH),
            environment=environment,
        )
        installer_dependencies = verify_windows_dependencies(
            {"installer_x64.exe": installer},
            environment=environment,
        )
        licenses = package / "licenses/windows-driver"
        licenses.mkdir(parents=True)
        for name in ("COPYING", "COPYING-LGPL"):
            shutil.copyfile(source / name, licenses / f"libwdi-{name}.txt")
        for name in ("License.rtf", "redist.txt.x64"):
            shutil.copyfile(redist / name, licenses / name)
        for name in ("COPYING", "COPYING.MinGW-w64-runtime.txt", "COPYING.MinGW-w64.txt"):
            shutil.copyfile(
                toolchain / "x86_64-w64-mingw32/share/mingw32" / name,
                licenses / f"mingw-w64-{name}",
            )
        shutil.copyfile(toolchain / "LICENSE.TXT", licenses / "llvm-LICENSE.txt")
        sources = package / "sources/windows-driver"
        sources.mkdir(parents=True)
        shutil.copyfile(archives["libwdi"], sources / archives["libwdi"].name)
        shutil.copyfile(Path(__file__), sources / "build.py")
        shutil.copyfile(source / "source-date-epoch.patch", sources / "source-date-epoch.patch")
        for name in ("windows-driver.lock.toml", "windows-host.lock.toml"):
            shutil.copyfile(common.ROOT / name, sources / name)
        notice = {
            name: {key: entry[key] for key in ("url", "sha256", "size")}
            for name, entry in {**lock, "llvm_mingw": toolchain_entry}.items()
        }
        (sources / "downloads.json").write_bytes(common.canonical_json_bytes(notice))
        (sources / "README.txt").write_text(
            "Windows driver helper source and build material\n\n"
            "libwdi is licensed under LGPL version 3 or later. Its complete source\n"
            "and original copyright notices are in the accompanying source archive.\n"
            "The supplied patch makes embedded resource timestamps honor\n"
            "SOURCE_DATE_EPOCH. The build recipe applies it to a fresh source tree.\n"
            "Microsoft redistributable components retain the accompanying license.\n\n"
            "On Linux, install GCC and 7-Zip. Download the pinned llvm_mingw and wdf\n"
            "inputs in downloads.json and verify their SHA-256 digests. Extract the\n"
            "toolchain and libwdi archives, then run:\n\n"
            "python3 build.py --toolchain <llvm-mingw-directory> \\\n"
            "  --source <libwdi-directory> --wdf <wdfcoinstaller.msi> \\\n"
            f"  --source-date-epoch {SOURCE_DATE_EPOCH} --output <wdi-simple.exe>\n\n"
            "The build runs only the native resource embedder. It does not execute\n"
            "Windows programs or install a driver on the build host.\n",
            encoding="utf-8",
        )
        files = {
            path.relative_to(package).as_posix(): path
            for path in package.rglob("*")
            if path.is_file()
        }
        dependencies = verify_windows_dependencies(files, environment=environment)
        destination = root / "package"
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(package, destination)
        result = {relative: destination / relative for relative in files}
        receipt = {
            "files": {
                relative: common.sha256_file(path) for relative, path in sorted(result.items())
            },
            "imports": dependencies,
            "installer_imports": installer_dependencies,
        }
        (root / "receipt.json").write_bytes(common.canonical_json_bytes(receipt))
        return result


def _main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--wdf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-date-epoch", required=True, type=int)
    args = parser.parse_args()
    _extract_redist(args.wdf.resolve(), args.source.resolve())
    _build_driver(
        toolchain=args.toolchain.resolve(),
        source=args.source.resolve(),
        output=args.output.resolve(),
        source_date_epoch=args.source_date_epoch,
    )


if __name__ == "__main__":
    _main()
