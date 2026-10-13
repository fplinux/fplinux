# SPDX-License-Identifier: GPL-2.0-only
"""Small real manifest trees for Linux discovery and workspace component tests."""

from __future__ import annotations

from pathlib import Path

INPUT_FILES = Path(__file__).with_name("linux-inputs")


def write(root: Path, relative: str, contents: str) -> Path:
    """Write one declared fixture input and create its parent directory."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    return path


def platform(root: Path, name: str, *, source_lock: str, arch: str) -> Path:
    """Declare Linux input operations without bootstrap, userspace, or loader metadata."""
    write(root, f"platforms/{name}/linux/overlay/drivers/{name}/common.c", "common driver\n")
    return write(
        root,
        f"platforms/{name}/platform.toml",
        (INPUT_FILES / "platform.toml.in").read_text(encoding="utf-8")
        % {"name": name, "name_upper": name.upper(), "source_lock": source_lock, "arch": arch},
    )


def target(root: Path, name: str, *, platform_name: str, arch: str = "arm") -> Path:
    """Declare board source files while leaving unrelated artifacts absent."""
    write(
        root, f"targets/{name}/linux/overlay/drivers/{platform_name}/{name}.c", f"{name} driver\n"
    )
    write(root, f"targets/{name}/linux/board.dts", f"{name} device tree\n")
    return write(
        root,
        f"targets/{name}/target.toml",
        (INPUT_FILES / "target.toml.in").read_text(encoding="utf-8")
        % {"name": name, "platform_name": platform_name, "arch": arch},
    )
