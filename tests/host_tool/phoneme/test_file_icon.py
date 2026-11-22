# SPDX-License-Identifier: GPL-2.0-only
"""The phoneME Files preview provider as a standalone process."""

from __future__ import annotations

import struct
import subprocess
import zlib
from typing import TYPE_CHECKING
from zipfile import ZIP_DEFLATED, ZipFile

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

PORT = ROOT / "alpine/aports/fplinux-phoneme"
IDENTITY = "MIDlet-Name: Test\nMIDlet-Vendor: Example\nMIDlet-Version: 1.0\n"


@pytest.fixture(scope="module")
def file_icon(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "phoneme-file-icon") as directory:
        executable = directory / "fplinux-phoneme-file-icon"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include'}",
                f"-I{ROOT / 'include/fplinux'}",
                str(PORT / "vm/fplinux-pm-file-icon.c"),
                str(PORT / "vm/fplinux-pm-jar.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                "-lz",
            ],
            name="compile the standalone phoneME Files preview provider",
        )
        yield executable


def png_chunk(kind: bytes, data: bytes) -> bytes:
    content = kind + data
    return struct.pack(">I", len(data)) + content + struct.pack(">I", zlib.crc32(content))


def png(width: int = 1, height: int = 1, *, shade: int = 0, comment_bytes: int = 0) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    scanline = b"\0" + bytes([shade]) * width
    comment = png_chunk(b"tEXt", b"Comment\0" + b"x" * comment_bytes) if comment_bytes else b""
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + comment
        + png_chunk(b"IDAT", zlib.compress(scanline * height))
        + png_chunk(b"IEND", b"")
    )


def create_jar(path: Path, attributes: str, icons: dict[str, bytes]) -> None:
    with ZipFile(path, "w", compression=ZIP_DEFLATED) as archive:
        archive.writestr(
            "META-INF/MANIFEST.MF", f"Manifest-Version: 1.0\n{IDENTITY}{attributes}\n"
        )
        for name, data in icons.items():
            archive.writestr(name, data)


def run_icon(executable: Path, *arguments: Path | str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(executable), *(str(argument) for argument in arguments)],
        capture_output=True,
        check=False,
        timeout=10,
    )


def test_prefers_suite_icon_at_dimension_limit(file_icon: Path, tmp_path: Path) -> None:
    jar = tmp_path / "Example.JAR"
    suite_icon = png(128, 128)
    create_jar(
        jar,
        "MIDlet-Icon: /icons/suite.png\nMIDlet-1: Test, /icons/midlet.png, example.Main\n",
        {"icons/suite.png": suite_icon, "icons/midlet.png": png(shade=255)},
    )

    result = run_icon(file_icon, jar)

    assert result.returncode == 0, result.stderr
    assert result.stdout == suite_icon


def test_explicit_jad_and_same_stem_jar_use_descriptor_icon(
    file_icon: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "Example App.jar"
    jad = tmp_path / "Example App.JAD"
    descriptor_icon = png(shade=128)
    create_jar(
        jar,
        "MIDlet-Icon: /suite.png\nMIDlet-1: Test, /midlet.png, example.Main\n",
        {"suite.png": png(), "midlet.png": png(shade=255), "descriptor.png": descriptor_icon},
    )
    jad.write_text(
        f"{IDENTITY}MIDlet-Jar-URL: Example%20App.jar\nMIDlet-Jar-Size: {jar.stat().st_size}\n"
        "MIDlet-Icon: /descriptor.png\n",
        encoding="utf-8",
    )

    for path in (jar, jad):
        result = run_icon(file_icon, path)
        assert result.returncode == 0, result.stderr
        assert result.stdout == descriptor_icon


@pytest.mark.parametrize(
    "suite_icon",
    [None, b"not a PNG", png(129, 1), png(1, 129), png(comment_bytes=65536)],
    ids=["missing", "unsupported", "too-wide", "too-tall", "too-many-bytes"],
)
def test_falls_back_to_first_midlet_icon(
    file_icon: Path, tmp_path: Path, suite_icon: bytes | None
) -> None:
    jar = tmp_path / "example.jar"
    midlet_icon = png(shade=255)
    icons = {"midlet.png": midlet_icon}
    if suite_icon is not None:
        icons["suite.png"] = suite_icon
    create_jar(
        jar,
        "MIDlet-Icon: /suite.png\nMIDlet-1: Test, /midlet.png, example.Main\n",
        icons,
    )

    result = run_icon(file_icon, jar)

    assert result.returncode == 0, result.stderr
    assert result.stdout == midlet_icon


def test_suite_without_first_midlet_icon_returns_no_preview(
    file_icon: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "example.jar"
    create_jar(
        jar,
        "MIDlet-1: First, , example.Main\nMIDlet-2: Second, /second.png, example.Other\n",
        {"second.png": png()},
    )

    result = run_icon(file_icon, jar)

    assert result.returncode == 1
    assert result.stdout == b""


def test_missing_suite_returns_no_preview(file_icon: Path, tmp_path: Path) -> None:
    result = run_icon(file_icon, tmp_path / "missing.jar")

    assert result.returncode == 1
    assert result.stdout == b""


@pytest.mark.parametrize("arguments", [(), ("relative.jar",), ("/first.jar", "/second.jar")])
def test_invalid_arguments_return_status_two(file_icon: Path, arguments: tuple[str, ...]) -> None:
    result = run_icon(file_icon, *arguments)

    assert result.returncode == 2
    assert result.stdout == b""
