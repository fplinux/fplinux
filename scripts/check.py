#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fast, read-only source quality gate."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import tomllib
from pathlib import Path
from typing import NoReturn

from fplinux_cli.config import discover_targets, load_platform, load_target

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_PARTS = {".cache", ".git", "out", "__pycache__"}
BINARY_SUFFIXES = {".bin", ".jpg", ".png", ".pyc", ".zip"}


def fail(message: str) -> NoReturn:
    raise SystemExit(f"check failed: {message}")


def run(command: list[str]) -> None:
    print("+", shlex.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def source_files() -> list[Path]:
    files: list[Path] = []
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        if path.name == ".fplinux-workspace":
            continue
        if path.is_symlink():
            fail(f"source symlink is not allowed: {relative}")
        if not path.is_file():
            continue
        if path.suffix in BINARY_SUFFIXES:
            fail(f"binary artifact is not allowed in source: {relative}")
        files.append(path)
    return files


def check_text(files: list[Path]) -> None:
    for path in files:
        relative = path.relative_to(ROOT)
        data = path.read_bytes()
        if b"\0" in data:
            fail(f"NUL byte in source file: {relative}")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as error:
            fail(f"source is not UTF-8: {relative}: {error}")
        if "\r" in text:
            fail(f"non-LF line ending: {relative}")
        if text and not text.endswith("\n"):
            fail(f"missing final newline: {relative}")
        for number, line in enumerate(text.splitlines(), 1):
            if line.endswith((" ", "\t")):
                fail(f"trailing whitespace: {relative}:{number}")
            if path.suffix != ".patch" and re.search(r" +\t", line):
                fail(f"space before tab: {relative}:{number}")


def check_container_policy(files: list[Path]) -> None:
    containerfiles = [path for path in files if path.name == "Containerfile"]
    dockerfiles = [path for path in files if path.name == "Dockerfile"]
    compose_files = [
        path
        for path in files
        if re.fullmatch(r"(?:docker-)?compose(?:\.[^.]+)?\.ya?ml", path.name)
    ]
    if len(containerfiles) != 1 or dockerfiles or compose_files:
        fail("source must contain exactly one Containerfile and no Dockerfile or compose file")
    instructions = [
        line.strip()
        for line in containerfiles[0].read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    if from_instructions != ["FROM ${BASE_IMAGE}"] or instructions[0] != "ARG BASE_IMAGE":
        fail("the toolchain Containerfile must use one lock-provided FROM ${BASE_IMAGE}")


def check_release_lock() -> None:
    """Validate the release verification lock against known targets."""
    with (ROOT / "releases.lock.toml").open("rb") as stream:
        lock = tomllib.load(stream)
    if set(lock) != {"verified"} or not isinstance(lock["verified"], dict):
        fail("releases.lock.toml must contain exactly one [verified] table")
    targets = set(discover_targets())
    for name, digest in lock["verified"].items():
        if name not in targets:
            fail(f"verified release entry is not a target: {name}")
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            fail(f"invalid verified runtime SHA256 for target: {name}")


def quality_sources(
    files: list[Path],
) -> tuple[list[str], list[str], list[str], list[str]]:
    python_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".py"]
    markdown_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".md"]
    posix_shell_files: list[str] = []
    bash_files: list[str] = []
    for path in files:
        if path.suffix not in {"", ".sh"}:
            continue
        with path.open(errors="strict") as stream:
            first_line = stream.readline().strip()
        relative = str(path.relative_to(ROOT))
        if first_line == "#!/usr/bin/env bash":
            bash_files.append(relative)
        elif first_line in {"#!/bin/sh", "#!/usr/bin/env sh"}:
            posix_shell_files.append(relative)
    return python_files, markdown_files, posix_shell_files, bash_files


def buildroot_sources(files: list[Path]) -> list[str]:
    """Select buildroot-external files understood by Buildroot check-package."""
    result = [
        str(path.relative_to(ROOT))
        for path in files
        if path.relative_to(ROOT).parts[0] == "buildroot-external"
        and (path.suffix in {".hash", ".mk"} or path.name == "Config.in")
    ]
    if not result:
        fail("no buildroot-external sources were discovered")
    return result


def userspace_c_sources(files: list[Path]) -> list[tuple[str, bool]]:
    """Discover in-tree userspace C and whether each source needs libusb."""
    result: dict[str, bool] = {}
    for path in files:
        relative = path.relative_to(ROOT)
        if relative.suffix == ".c" and relative.parts[:2] == (
            "buildroot-external",
            "package",
        ):
            result[relative.as_posix()] = False

    platform_names = {load_target(target)["platform"] for target in discover_targets()}
    for platform_name in sorted(platform_names):
        platform = load_platform(platform_name)
        for recipe in platform["host"]["tools"]:
            if recipe["type"] != "cc-libusb/v1":
                continue
            source = recipe["source"]
            path = ROOT / source
            if path.suffix != ".c" or path.is_symlink() or not path.is_file():
                fail(f"cc-libusb/v1 source must be a regular C file: {source}")
            result[source] = True

    if not result:
        fail("no userspace C sources were discovered")
    return sorted(result.items())


def run_userspace_analysis(output: Path, sources: list[tuple[str, bool]]) -> None:
    """Run Clang Static Analyzer for every discovered userspace C source."""
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        fail(f"invalid scan-build output path: {output}")
    output.mkdir(parents=True, exist_ok=True)
    common_flags = [
        "-std=c11",
        "-O2",
        "-g0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fno-ident",
        "-c",
    ]
    needs_libusb = any(libusb for _source, libusb in sources)
    libusb_flags: list[str] = []
    if needs_libusb:
        pkg_config = subprocess.run(
            ["pkg-config", "--cflags", "libusb-1.0"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if pkg_config.returncode:
            fail(pkg_config.stderr.strip() or "pkg-config could not resolve libusb-1.0")
        libusb_flags = shlex.split(pkg_config.stdout)

    analyzer = [
        "scan-build",
        "--status-bugs",
        "--use-cc=clang",
        "-o",
        str(output / "reports"),
        "clang",
    ]
    for index, (source, requires_libusb) in enumerate(sources):
        flags = [*common_flags]
        if requires_libusb:
            flags.extend(libusb_flags)
            flags.append("-pthread")
        run(
            [
                *analyzer,
                *flags,
                source,
                "-o",
                str(output / f"{index:02d}-{Path(source).stem}.o"),
            ]
        )


def check_userspace_c(sources: list[tuple[str, bool]]) -> None:
    """Select persistent check output when the container provides it."""
    persistent = os.environ.get("FPLINUX_SCAN_BUILD_DIR")
    if persistent is not None:
        output = Path(persistent)
        if not output.is_absolute():
            fail("FPLINUX_SCAN_BUILD_DIR must be absolute")
        run_userspace_analysis(output, sources)
        return
    with tempfile.TemporaryDirectory(prefix="fplinux-scan-build-") as temporary:
        run_userspace_analysis(Path(temporary), sources)


def main() -> None:
    files = source_files()
    check_text(files)
    check_container_policy(files)
    check_release_lock()
    python_files, markdown_files, posix_shell_files, bash_files = quality_sources(files)
    toml_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".toml"]
    json_files = [
        str(path.relative_to(ROOT))
        for path in files
        if path.suffix in {".json", ".jsonc"} and path.name != "package-lock.json"
    ]
    bootstrap_c = [
        str(path.relative_to(ROOT))
        for path in files
        if path.suffix in {".c", ".h"} and "bootstrap" in path.relative_to(ROOT).parts
    ]
    c_sources = userspace_c_sources(files)
    buildroot_files = buildroot_sources(files)

    run(["taplo", "check", *toml_files])
    run(["taplo", "fmt", "--check", *toml_files])
    run(["prettier", "--check", "--ignore-unknown", *markdown_files, *json_files])
    run(["markdownlint-cli2"])
    text_files = [
        str(path.relative_to(ROOT))
        for path in files
        if path.suffix == ".txt" and path.relative_to(ROOT).parts[0] != "LICENSES"
    ]
    run(["vale", "--config", ".vale.ini", *markdown_files, *text_files])
    run(["typos", "."])
    run(["gitleaks", "dir", "--no-banner", "--redact", "--exit-code", "1", "."])
    run(["reuse", "lint"])
    run(["ruff", "check", *python_files])
    run(["ruff", "format", "--check", *python_files])
    run(["mypy", *python_files])
    run(["shfmt", "-d", "-ln", "posix", *posix_shell_files])
    run(["shfmt", "-d", "-ln", "bash", *bash_files])
    run(
        [
            "shellcheck",
            "--enable=all",
            "--severity=warning",
            *posix_shell_files,
            *bash_files,
        ]
    )
    # Podman's OCI output has no SHELL support, so pipefail cannot be enabled
    # (DL4006); every pipe feeds printf output into a checked sha256sum.
    run(["hadolint", "--ignore", "DL4006", "toolchains/Containerfile"])
    # The quality venv python cannot import check-package's flake8 and magic
    # dependencies; they are provided for the system interpreter.
    run(
        [
            "/usr/bin/python3",
            "/opt/buildroot/utils/check-package",
            "--br2-external",
            *buildroot_files,
        ]
    )
    run(
        [
            "clang-format",
            "--style=file",
            "--dry-run",
            "--Werror",
            *(source for source, _requires_libusb in c_sources),
            *bootstrap_c,
        ]
    )
    check_userspace_c(c_sources)

    print(f"source checks: OK ({len(files)} UTF-8 source files)")


if __name__ == "__main__":
    main()
