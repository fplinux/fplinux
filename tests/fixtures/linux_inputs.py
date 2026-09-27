# SPDX-License-Identifier: GPL-2.0-only
"""Small real manifest trees for Linux discovery and workspace component tests."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def write(root: Path, relative: str, contents: str) -> Path:
    """Write one declared fixture input and create its parent directory."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    return path


def platform(root: Path, name: str, *, source_lock: str, arch: str) -> Path:
    """Declare Linux input operations without bootstrap, userspace, or loader metadata."""
    write(root, f"platforms/{name}/common.c", "common driver\n")
    return write(
        root,
        f"platforms/{name}/platform.toml",
        f"""[identity]
vendor = "Example"
soc = "{name.upper()}"
aliases = []
compatible = "example,{name}"

[linux]
source_lock = "{source_lock}"
arch = "{arch}"
cross_compile = "{arch}-linux-gnu-"
dts_directory = "arch/{arch}/boot/dts"
platform_identity_header = "arch/{arch}/include/{name}.h"
patches = []
appends = []

[[linux.copies]]
source = "platforms/{name}/common.c"
destination = "drivers/{name}/common.c"
""",
    )


def target(root: Path, name: str, *, platform_name: str, arch: str = "arm") -> Path:
    """Declare board source files while leaving unrelated artifacts absent."""
    write(root, f"targets/{name}/linux/board.c", f"{name} driver\n")
    write(root, f"targets/{name}/linux/board.dts", f"{name} device tree\n")
    return write(
        root,
        f"targets/{name}/target.toml",
        f"""platform = "{platform_name}"

[identity]
brand = "Example"
product = "{name}"
hardware_codes = []
compatible = "example,{name}"

[linux]
patches = []
appends = []

[[linux.copies]]
source = "linux/board.c"
destination = "drivers/{platform_name}/{name}.c"

[[linux.copies]]
source = "linux/board.dts"
destination = "arch/{arch}/boot/dts/{name}.dts"

[microsd]
linux_patches = []
""",
    )
