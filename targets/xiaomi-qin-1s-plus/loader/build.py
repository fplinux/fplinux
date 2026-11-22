# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001 -- the build loads this recipe from the target source directory.
"""Build the Xiaomi Qin 1s+ download stages for the SC9820E chain."""

from __future__ import annotations

import shutil
import textwrap
from pathlib import Path
from typing import Any

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build

_LLVM = "/usr/lib/llvm23/bin/"


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
            "-D__ASSEMBLER__",
            "-I",
            "include",
            "-I",
            "include/arch/aarch64",
            "-I",
            "plat/qin1splus/include",
            str(check),
            "-o",
            str(source / "fplinux-placement.i"),
        ],
        cwd=source,
    )
    compiler = f"{_LLVM}clang --target=aarch64-none-elf"
    process_build.run(
        [
            "make",
            f"-j{jobs}",
            f"BUILD_BASE={build}",
            "PLAT=qin1splus",
            "ARCH=aarch64",
            "DEBUG=0",
            f"CC={compiler}",
            f"AS={compiler}",
            f"CPP={compiler}",
            f"AR={_LLVM}llvm-ar",
            "LD=ld.lld",
            f"OC={_LLVM}llvm-objcopy",
            f"OD={_LLVM}llvm-objdump",
            "bl31",
        ],
        cwd=source,
    )


def build_stage(
    role: str,
    source: Path,
    build: Path,
    target_config: dict[str, Any],
    *,
    jobs: int,
) -> tuple[Path, Path, Path]:
    """Apply the board patch and return the linked ELF, binary and map."""
    target_source = Path(__file__).parent
    patches = {"fdl1": "chipram.patch", "fdl2": "u-boot.patch", "sml": "trusted-firmware.patch"}
    sources_build.apply_patches(source, [inputs_build.require_file(target_source / patches[role])])
    loader = target_config["loader"]
    if role == "fdl1":
        _build_fdl1(source, build, sml_entry=loader["sml_load"], jobs=jobs)
        return (
            build / "nand_fdl/fdl1.axf",
            build / "nand_fdl/fdl1.bin",
            build / "nand_fdl/fdl1.map",
        )
    if role == "fdl2":
        _build_fdl2(
            source, build, entry=target_config["bootstrap"]["layout"]["entry_load"], jobs=jobs
        )
        return build / "u-boot", build / "u-boot-dtb.bin", build / "u-boot.map"
    shutil.copytree(
        inputs_build.require_directory(target_source / "bl31"), source / "plat/qin1splus"
    )
    _build_sml(source, build, loader, jobs=jobs)
    base = build / "qin1splus/release"
    return base / "bl31/bl31.elf", base / "bl31.bin", base / "bl31/bl31.map"
