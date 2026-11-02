#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Produce the bounded files consumed by the host-process U-Boot tests."""

import pathlib
import shutil
import subprocess
import sys

output = pathlib.Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
elf = bytearray(52)
elf[0:4] = b"\x7fELF"
elf[4:7] = b"\x01\x01\x01"
elf[16:18] = (2).to_bytes(2, "little")
elf[18:20] = (40).to_bytes(2, "little")
elf[20:24] = (1).to_bytes(4, "little")
elf[24:28] = (0x81000000).to_bytes(4, "little")
(output / "u-boot").write_bytes(bytes(elf) + b"DEBUG")
(output / "u-boot-dtb.bin").write_bytes(b"binary")

target_header = pathlib.Path("include/fplinux-uboot-target.h")
if target_header.exists():
    target = subprocess.run(
        ["cc", "-E", "-P", "-I", "include", "-"],
        input=b'#include "fplinux-uboot-target.h"\nFPLINUX_UBOOT_TARGET\n',
        capture_output=True,
        check=True,
        timeout=30,
    ).stdout
    (output / "u-boot.dtb").write_bytes(target)
else:
    (output / "u-boot.dtb").write_bytes(b"dtb")

(output / "u-boot.map").write_text(
    (output / ".config").read_text(encoding="utf-8"),
    encoding="utf-8",
)
tools = output / "tools"
tools.mkdir()
for name in ("mkimage", "dumpimage"):
    tool = tools / name
    shutil.copyfile("scripts/tool-version.py", tool)
    tool.chmod(0o755)
