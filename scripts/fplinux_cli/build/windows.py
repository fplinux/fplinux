# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the pinned Windows x86-64 dependencies for standalone archives."""

from __future__ import annotations

import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.build.host import copy_host_project_files, extract_host_members
from fplinux_cli.build.host_package import copy_mingw_licenses, publish_host_package
from fplinux_cli.build.sources import source_lock_entry
from fplinux_cli.build.windows_environment import WindowsBuildEnvironment
from fplinux_cli.common import canonical_json_bytes, fail
from fplinux_cli.device_data.prepare import NAND_ECC_TOOL
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.values import relative_value

_SYSTEM_DLLS = frozenset(
    {
        "advapi32.dll",
        "bcrypt.dll",
        "cfgmgr32.dll",
        "crypt32.dll",
        "dbghelp.dll",
        "dnsapi.dll",
        "gdi32.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "msvcrt.dll",
        "ncrypt.dll",
        "netapi32.dll",
        "newdev.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "powrprof.dll",
        "propsys.dll",
        "psapi.dll",
        "rpcrt4.dll",
        "secur32.dll",
        "setupapi.dll",
        "shell32.dll",
        "shlwapi.dll",
        "ucrtbase.dll",
        "user32.dll",
        "userenv.dll",
        "version.dll",
        "winmm.dll",
        "ws2_32.dll",
        "wtsapi32.dll",
    }
)


def _download(entry: dict[str, Any], directory: Path, *, offline: bool) -> Path:
    return download_locked_file(
        entry["url"],
        entry["sha256"],
        directory / entry["name"],
        offline=offline,
        size=entry["size"],
    )


def _extract_zip(
    archive: Path,
    destination: Path,
    *,
    prefix: str = "",
    members: set[str] | None = None,
) -> None:
    """Project files from a locked ZIP without accepting absolute or parent paths."""
    with zipfile.ZipFile(archive) as source:
        for member in source.infolist():
            if member.is_dir() or not member.filename.startswith(prefix):
                continue
            relative = member.filename.removeprefix(prefix)
            if members is not None and relative not in members:
                continue
            path = destination / relative_value(relative, "Windows archive member")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source.read(member))


def verify_windows_dependencies(
    files: dict[str, Path],
    *,
    environment: WindowsBuildEnvironment | None = None,
) -> dict[str, list[str]]:
    """Require x86-64 PE files and a complete application-local DLL closure."""
    imports: dict[str, list[str]] = {}
    for relative, path in sorted(files.items()):
        if path.suffix.lower() not in {".exe", ".dll", ".pyd"}:
            continue
        if environment is None:
            inspected = subprocess.run(
                ["objdump", "-p", str(path)],
                capture_output=True,
                text=True,
                check=False,
            )
        else:
            inspected = environment.inspect(path)
        if inspected.returncode or "file format pei-x86-64" not in inspected.stdout:
            fail(f"Windows dependency is not an x86-64 PE: {relative}")
        dependencies = re.findall(r"DLL Name:\s*(\S+)", inspected.stdout)
        siblings = {other.name.lower() for other in files.values() if other.parent == path.parent}
        for dependency in dependencies:
            name = dependency.lower()
            if (
                name not in _SYSTEM_DLLS
                and not name.startswith(("api-ms-win-", "ext-ms-win-"))
                and name not in siblings
            ):
                fail(f"Windows dependency is missing beside {relative}: {dependency}")
        imports[relative] = dependencies
    return imports


def _build_tools(  # noqa: PLR0913 -- compile paths and offline policy are distinct inputs.
    *,
    compiler: Path,
    libusb: Path,
    downloads: Path,
    work: Path,
    package: Path,
    offline: bool,
    environment: WindowsBuildEnvironment,
    nand_ecc: bool,
) -> None:
    sources = common.load_toml(common.ROOT / "sources.lock.toml")
    recipes = load_platform("ums9117")["host"]["tools"]
    include = libusb / "include" / "libusb-1.0"
    include.mkdir()
    shutil.copyfile(libusb / "include" / "libusb.h", include / "libusb.h")
    tools = package / "tools"
    tools.mkdir(parents=True)
    source_files: dict[str, Path] = {}
    required_tools = {"spd_dump", "libc_server", "fphelper_t117"}
    if nand_ecc:
        required_tools.add(NAND_ECC_TOOL)
    for recipe in recipes:
        if recipe["name"] not in required_tools:
            continue
        lock = source_lock_entry(sources, recipe["source_lock"])
        archive = download_locked_file(
            lock["archive_url"],
            lock["archive_sha256"],
            downloads / recipe["cache_name"],
            offline=offline,
        )
        source = work / recipe["source_directory"]
        prefix = recipe["archive_prefix"].replace("{commit}", lock["commit"])
        extract_host_members(archive, prefix, recipe["members"], lock["files"], source)
        if recipe["source_lock"] == "fpdoom_host":
            with tarfile.open(archive) as upstream:
                license_stream = upstream.extractfile(prefix.split("/", 1)[0] + "/LICENSE")
                if license_stream is None:
                    fail("host source license is missing")
                license_contents = license_stream.read()
            (source / "LICENSE").write_bytes(license_contents)
            license_directory = package / "licenses/windows-host"
            license_directory.mkdir(parents=True, exist_ok=True)
            (license_directory / "fpdoom-Unlicense.txt").write_bytes(license_contents)
        copy_host_project_files(source, recipe["copies"])
        environment.apply_patches(
            source,
            [common.ROOT / relative for relative in recipe["patches"]],
        )
        is_ecc = recipe["name"] == NAND_ECC_TOOL
        main = "main.c" if recipe["name"] == "fphelper_t117" else f"{recipe['name']}.c"
        output = tools / f"{recipe['name']}.exe"
        command = [
            str(compiler),
            "-s",
            "-O2",
            "-g0",
            "-Wall",
            "-Wextra",
            "-std=c11" if is_ecc else "-std=c99",
            "-pedantic",
            "-fno-ident",
            "-Wl,--no-insert-timestamp",
            "-o",
            str(output),
            main,
        ]
        if is_ecc:
            command.extend(["bch.c", "fplinux-cli.c", "-lws2_32"])
            license_directory = package / "licenses/windows-host"
            license_directory.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / "COPYING", license_directory / "ums9117-nand-ecc-COPYING.txt")
            source_command = [*command]
            source_command[0] = compiler.name
            source_command[source_command.index("-o") + 1] = output.name
            (source / "BUILD-WINDOWS.txt").write_text(
                f"Build with the x86-64 LLVM-MinGW UCRT compiler:\n{shlex.join(source_command)}\n",
                encoding="utf-8",
            )
        elif recipe["name"] != "fphelper_t117":
            command.extend(
                [
                    "-DUSE_LIBUSB=1",
                    f"-I{libusb / 'include'}",
                    str(libusb / "MinGW64/static/libusb-1.0.dll.a"),
                ]
            )
        subprocess.run(command, cwd=source, check=True)
        rebuilt = work / f"{recipe['name']}-rebuild.exe"
        command[command.index("-o") + 1] = str(rebuilt)
        subprocess.run(command, cwd=source, check=True)
        if output.read_bytes() != rebuilt.read_bytes():
            fail(f"Windows tool build is not reproducible: {recipe['name']}")
        for path in source.iterdir():
            if path.is_file():
                source_files[f"{recipe['name']}/{path.name}"] = path
    source_archive = package / "sources/windows-tools-source.zip"
    source_archive.parent.mkdir(parents=True)
    with zipfile.ZipFile(source_archive, "w", compression=zipfile.ZIP_DEFLATED) as source_zip:
        for relative, path in sorted(source_files.items()):
            member = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
            member.compress_type = zipfile.ZIP_DEFLATED
            source_zip.writestr(member, path.read_bytes())


def _copy_licenses(toolchain: Path, libusb_source: Path, package: Path) -> None:
    licenses = package / "licenses/windows-host"
    licenses.mkdir(parents=True, exist_ok=True)
    copy_mingw_licenses(toolchain, licenses)
    shutil.copyfile(common.ROOT / "LICENSES/Unlicense.txt", licenses / "Unlicense.txt")
    with tarfile.open(libusb_source) as archive:
        stream = archive.extractfile("libusb-1.0.29/COPYING")
        if stream is None:
            fail("libusb license is missing")
        (licenses / "libusb-COPYING.txt").write_bytes(stream.read())
    shutil.copyfile(libusb_source, package / "sources" / libusb_source.name)


def prepare_windows_host(*, cache: Path, offline: bool, nand_ecc: bool = False) -> dict[str, Path]:
    """Build and collect Windows files while the caller owns the project cache lock."""
    cache = cache.resolve()
    lock = common.load_toml(common.ROOT / "windows-host.lock.toml")
    root = cache / "windows-host"
    downloads = root / "downloads"
    archives = {name: _download(entry, downloads, offline=offline) for name, entry in lock.items()}
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root, prefix=".build-") as temporary:
        work = Path(temporary)
        patches = [
            common.ROOT / relative
            for recipe in load_platform("ums9117")["host"]["tools"]
            if recipe["name"] in {"spd_dump", "libc_server", "fphelper_t117"}
            for relative in recipe["patches"]
        ]
        environment = WindowsBuildEnvironment(work=work, inputs=[*archives.values(), *patches])
        toolchain_root = work / "toolchain"
        toolchain_root.mkdir()
        with tarfile.open(archives["llvm_mingw"]) as archive:
            archive.extractall(toolchain_root, filter="data")
        toolchain = toolchain_root / lock["llvm_mingw"]["directory"]
        libusb = work / "libusb"
        environment.run(
            [
                "7z",
                "x",
                "-y",
                f"-o{libusb}",
                str(archives["libusb"]),
                "include/libusb.h",
                "MinGW64/dll/libusb-1.0.dll",
                "MinGW64/static/libusb-1.0.dll.a",
            ],
            cwd=work,
        )
        package = work / "package"
        _build_tools(
            compiler=toolchain / "bin/x86_64-w64-mingw32-gcc",
            libusb=libusb,
            downloads=cache / "downloads",
            work=work,
            package=package,
            offline=offline,
            environment=environment,
            nand_ecc=nand_ecc,
        )
        shutil.copyfile(libusb / "MinGW64/dll/libusb-1.0.dll", package / "tools/libusb-1.0.dll")
        _extract_zip(archives["python"], package / "runtime")
        (package / "runtime/python314._pth").write_text(
            "python314.zip\n.\n..\\common\n..\\scripts\n..\n",
            encoding="utf-8",
        )
        _extract_zip(
            archives["openssh"],
            package / "tools/openssh",
            prefix="OpenSSH-Win64/",
            members={
                "ssh.exe",
                "ssh-keygen.exe",
                "ssh-keyscan.exe",
                "libcrypto.dll",
                "LICENSE.txt",
                "NOTICE.txt",
            },
        )
        _copy_licenses(toolchain, archives["libusb_source"], package)
        notice = {
            name: {key: entry[key] for key in ("url", "sha256", "size")}
            for name, entry in lock.items()
        }
        (package / "sources/windows-host-downloads.json").write_bytes(canonical_json_bytes(notice))
        files = {
            path.relative_to(package).as_posix(): path
            for path in package.rglob("*")
            if path.is_file()
        }
        verify_windows_dependencies(files, environment=environment)
        return publish_host_package(package, root / "package", relative_paths=files)
