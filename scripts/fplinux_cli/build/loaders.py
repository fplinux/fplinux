# SPDX-License-Identifier: GPL-2.0-only
"""Build the SC9820E RAM download chain from pinned source archives."""

from __future__ import annotations

import json
import shutil
import struct
import tarfile
import tempfile
import textwrap
from dataclasses import asdict
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts import records
from fplinux_cli.build import environment as build_env
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.bootstrap import recipe as bootstrap_recipe
from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_bytes
from fplinux_cli.manifests import loaders as loader_manifest
from fplinux_cli.manifests.loaders import loader_sources

if TYPE_CHECKING:
    from fplinux_cli.manifests.loaders import LoaderSource

_RECEIPT_NAME = ".fplinux-loader-receipt.json"
_LLVM = "/usr/lib/llvm22/bin/"
_OUTPUTS = (
    "fdl1.bin",
    "fdl2.bin",
    "sml.bin",
    "fdl1.elf",
    "fdl2.elf",
    "sml.elf",
    "fdl1.map",
    "fdl2.map",
    "sml.map",
)


def _recipe(
    selected: tuple[LoaderSource, ...], target_source: Path, target_config: dict[str, Any]
) -> str:
    loader = target_config["loader"]
    environment_recipe, environment_content = inputs_build.environment_identity()
    implementation = {}
    for module in (
        common,
        records,
        build_env,
        inputs_build,
        process_build,
        sources_build,
        loader_manifest,
        bootstrap_recipe,
    ):
        module_path = module.__file__
        if module_path is None:
            fail(f"RAM-loader build module has no source file: {module.__name__}")
        implementation[module.__name__] = common.sha256_file(Path(module_path))
    manifest = {
        "sources": [asdict(source) for source in selected],
        "target_source": bootstrap_recipe.bootstrap_tree_entries(target_source),
        "placement": {
            "fdl1": loader["fdl1_load"],
            "fdl2": loader["fdl2_load"],
            "sml": loader["sml_load"],
            "entry": target_config["bootstrap"]["layout"]["entry_load"],
        },
        "environment": {"source_recipe": environment_recipe, "content": environment_content},
        "source_date_epoch": build_env.SOURCE_DATE_EPOCH,
        "implementation": implementation,
        "producer": common.sha256_file(Path(__file__)),
    }
    return sha256_bytes(canonical_json_bytes(manifest))


def _receipt(recipe: str, output: Path) -> dict[str, object]:
    return {
        "recipe": recipe,
        "outputs": {
            name: records.regular_file_record(output / name, "RAM-loader output", ValueError)
            for name in _OUTPUTS
        },
    }


def _cache_hit(recipe: str, output: Path) -> bool:
    try:
        return records.receipt_matches(output / _RECEIPT_NAME, _receipt(recipe, output))
    except ValueError:
        return False


def _extract_source(source: LoaderSource, destination: Path) -> Path:
    archive = sources_build.fetch(
        source.url, source.sha256, inputs_build.CACHE / "downloads", source.cache_name
    )
    prefix = PurePosixPath(source.archive_prefix)
    with tarfile.open(archive, "r:gz") as stream:
        members = []
        for member in stream.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or not path.is_relative_to(prefix):
                fail(f"unexpected RAM-loader archive member: {member.name}")
            relative = path.relative_to(prefix).as_posix()
            if source.role == "fdl1" and relative in {
                "imgheaderinsert",
                "secure/sansa/lib/libdx_sbrom.a",
            }:
                continue
            members.append(member)
        stream.extractall(destination, members=members, filter="data")
    return inputs_build.require_directory(destination / source.archive_prefix)


def _elf_entry(path: Path, *, bits: int, machine: int, expected: int) -> None:
    header = inputs_build.require_file(path).read_bytes()[:64]
    elf_class = 2 if bits == 64 else 1
    if (
        len(header) < 64
        or header[:6] != b"\x7fELF" + bytes((elf_class, 1))
        or struct.unpack_from("<H", header, 18)[0] != machine
        or struct.unpack_from("<Q" if bits == 64 else "<I", header, 24)[0] != expected
    ):
        fail(f"RAM-loader ELF entry or architecture differs from target placement: {path.name}")


def _dhtb(payload: bytes) -> bytes:
    if not payload or len(payload) > 0xFFFFFFFF:
        fail("RAM-loader payload must have a non-empty 32-bit size")
    header = bytearray(512)
    header[:8] = b"DHTB\x01\0\0\0"
    header[8:40] = bytes.fromhex(sha256_bytes(payload))
    struct.pack_into("<I", header, 0x30, len(payload))
    return bytes(header) + payload


def _build_fdl1(source: Path, build: Path, *, sml_entry: int, jobs: int) -> None:
    compiler = (
        f"{_LLVM}clang --target=aarch64-none-elf -std=gnu89 "
        "-Wno-int-conversion -Wno-error=return-mismatch "
        f"-DCONFIG_QIN_RAM_SML_ENTRY=0x{sml_entry:x}"
    )
    make = [
        "make",
        f"O={build}",
        f"CC={compiler}",
        f"CPP={compiler} -E",
        "LD=ld.lld",
        f"AR={_LLVM}llvm-ar",
        f"NM={_LLVM}llvm-nm",
        f"OBJCOPY={_LLVM}llvm-objcopy",
        f"OBJDUMP={_LLVM}llvm-objdump",
        f"STRIP={_LLVM}llvm-strip",
        "PLATFORM_RELFLAGS=-fno-common -mstrict-align",
        "PLATFORM_LIBGCC=",
    ]
    process_build.run([*make, "sp9820e_1h10_config"], cwd=source)
    process_build.run([*make, "-B", f"-j{jobs}", "fdl1"], cwd=source)


def _build_fdl2(source: Path, build: Path, *, entry: int, jobs: int) -> None:
    make = [
        "make",
        "-C",
        str(source),
        f"O={build}",
        "ARCH=arm",
        "CROSS_COMPILE=arm-none-eabi-",
        "DTC=dtc",
        f"KCFLAGS=-std=gnu89 -fmacro-prefix-map={source}=. -DCONFIG_QIN_RAM_ENTRY=0x{entry:x}",
        "HOSTCFLAGS=-O2 -std=gnu89",
    ]
    process_build.run([*make, "sp9820e_1h10_defconfig"])
    process_build.run(
        [
            *make,
            "DEVICE_TREE=sp9820e_1h10",
            f"-j{jobs}",
            "u-boot.bin",
            "u-boot.dtb",
            "u-boot-dtb.bin",
        ]
    )


def _build_sml(source: Path, build: Path, loader: dict[str, Any], *, jobs: int) -> None:
    check = source / "fplinux-placement.c"
    placement_check = textwrap.dedent("""\
        #include <platform_def.h>
        #if BL31_BASE != {sml_load:#x}
        #error "secure monitor base differs from target placement"
        #endif
        #if QIN_NS_ENTRY != {fdl2_entry:#x}
        #error "non-secure entry differs from target placement"
        #endif
        """)
    check.write_text(
        placement_check.format(sml_load=loader["sml_load"], fdl2_entry=loader["fdl2_load"] + 512),
        encoding="ascii",
    )
    process_build.run(
        [
            f"{_LLVM}clang",
            "--target=aarch64-none-elf",
            "-E",
            "-D__ASSEMBLY__",
            "-I",
            "include",
            "-I",
            "include/common",
            "-I",
            "include/plat/common",
            "-I",
            "include/lib",
            "-I",
            "include/lib/aarch64",
            "-I",
            "plat/qin1splus/include",
            str(check),
            "-o",
            str(source / "fplinux-placement.i"),
        ],
        cwd=source,
    )
    compiler = f"{_LLVM}clang --target=aarch64-none-elf"
    compatibility = "-Wno-error=unused-command-line-argument"
    process_build.run(
        [
            "make",
            f"-j{jobs}",
            f"BUILD_BASE={build}",
            "PLAT=qin1splus",
            "ARCH=aarch64",
            "DEBUG=0",
            f"CC={compiler} {compatibility} -Wno-error=deprecated-non-prototype",
            f"AS={compiler} {compatibility}",
            f"CPP={compiler} {compatibility} -E",
            f"PP={compiler} {compatibility} -E",
            f"AR={_LLVM}llvm-ar",
            "LD=ld.lld",
            f"OC={_LLVM}llvm-objcopy",
            f"OD={_LLVM}llvm-objdump",
            f"NM={_LLVM}llvm-nm",
            "bl31",
        ],
        cwd=source,
    )


def _collect_output(
    role: str, build: Path, destination: Path, target_config: dict[str, Any]
) -> None:
    loader = target_config["loader"]
    if role == "fdl1":
        elf, binary, map_file = (
            build / "nand_fdl/fdl1.axf",
            build / "nand_fdl/fdl1.bin",
            build / "nand_fdl/fdl1.map",
        )
        _elf_entry(elf, bits=64, machine=183, expected=loader["fdl1_load"] + 512)
    elif role == "fdl2":
        elf, binary, map_file = build / "u-boot", build / "u-boot-dtb.bin", build / "u-boot.map"
        _elf_entry(elf, bits=32, machine=40, expected=loader["fdl2_load"] + 512)
    else:
        base = build / "qin1splus/release"
        elf, binary, map_file = base / "bl31/bl31.elf", base / "bl31.bin", base / "bl31/bl31.map"
        _elf_entry(elf, bits=64, machine=183, expected=loader["sml_load"])
    payload = inputs_build.require_file(binary).read_bytes()
    if not payload:
        fail(f"source-built RAM-loader is empty: {role}")
    (destination / f"{role}.bin").write_bytes(payload if role == "sml" else _dhtb(payload))
    shutil.copyfile(elf, destination / f"{role}.elf")
    shutil.copyfile(inputs_build.require_file(map_file), destination / f"{role}.map")


def _build_chain(
    selected: tuple[LoaderSource, ...],
    target_source: Path,
    target_config: dict[str, Any],
    temporary: Path,
    *,
    jobs: int,
) -> Path:
    output = temporary / "publish"
    output.mkdir()
    patches = {"fdl1": "chipram.patch", "fdl2": "u-boot.patch", "sml": "trusted-firmware.patch"}
    for source_pin in selected:
        role = source_pin.role
        source = _extract_source(source_pin, temporary / role)
        sources_build.apply_patches(
            source, [inputs_build.require_file(target_source / patches[role])]
        )
        build = temporary / f"{role}-build"
        if role == "fdl1":
            _build_fdl1(source, build, sml_entry=target_config["loader"]["sml_load"], jobs=jobs)
        elif role == "fdl2":
            _build_fdl2(
                source,
                build,
                entry=target_config["bootstrap"]["layout"]["entry_load"],
                jobs=jobs,
            )
        else:
            shutil.copytree(
                inputs_build.require_directory(target_source / "bl31"), source / "plat/qin1splus"
            )
            _build_sml(source, build, target_config["loader"], jobs=jobs)
        _collect_output(role, build, output, target_config)
    for artifact in output.iterdir():
        artifact.chmod(0o644)
    return output


def _write_asset_lock(
    path: Path,
    recipe: str,
    selected: tuple[LoaderSource, ...],
    outputs: dict[str, tuple[str, str]],
) -> None:
    license_tag = "SPDX-License-" + "Identifier"
    lines = [f"# {license_tag}: GPL-2.0-only", f"recipe = {json.dumps(recipe)}"]
    for source in selected:
        relative, digest = outputs[source.role]
        lines.extend(("", "[[source]]"))
        for field, value in (
            ("role", source.role),
            ("url", source.url),
            ("archive_sha256", source.sha256),
            ("path", relative),
            ("sha256", digest),
        ):
            lines.append(f"{field} = {json.dumps(value)}")
    replace_file_atomically(path, ("\n".join(lines) + "\n").encode(), 0o644, sync=False)


def build_loaders(  # noqa: PLR0913 -- source, target, output and build policy stay explicit.
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    *,
    work: Path,
    jobs: int,
    lock_output: Path,
) -> dict[str, tuple[str, str]]:
    """Build or reuse the source chain and publish the three ready-to-upload assets."""
    if jobs < 1:
        fail("RAM-loader build jobs must be positive")
    build = target_config["loader"]["build"]
    selected = loader_sources(build, sources)
    target_source = inputs_build.require_directory(
        inputs_build.target_source(target, build["source"])
    )
    recipe = _recipe(selected, target_source, target_config)
    output = work / "loaders"
    if not _cache_hit(recipe, output):
        work.mkdir(parents=True, exist_ok=True)
        # The pinned monitor build strips '/.' while normalizing output directories.
        with tempfile.TemporaryDirectory(dir=work, prefix="loaders-") as temporary:
            staging = _build_chain(
                selected, target_source, target_config, Path(temporary), jobs=jobs
            )
            records.write_canonical_json(
                staging / _RECEIPT_NAME, _receipt(recipe, staging), mode=0o644
            )
            if output.exists() or output.is_symlink():
                inputs_build.require_directory(output)
                shutil.rmtree(output)
            staging.replace(output)
    if not _cache_hit(recipe, output):
        fail("source-built RAM-loader receipt is not reusable")
    assets = work / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    result = {}
    for role in ("fdl1", "fdl2", "sml"):
        relative = f"{role}.bin"
        data = inputs_build.require_file(output / relative).read_bytes()
        replace_file_atomically(assets / relative, data, 0o644, sync=False)
        result[role] = (relative, sha256_bytes(data))
    _write_asset_lock(lock_output, recipe, selected, result)
    return result
