#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fast, read-only source quality gate."""

from __future__ import annotations

import argparse
import os
import re
import shlex
import signal
import subprocess
import tempfile
import tomllib
from contextlib import contextmanager, suppress
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, NoReturn
from urllib.parse import unquote

from fplinux_cli import alpine_state
from fplinux_cli.config import discover_targets, load_platform, load_target
from fplinux_cli.output import RunReporter, current_stage, run_entrypoint

if TYPE_CHECKING:
    from collections.abc import Iterator

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_PARTS = {".cache", ".git", "__pycache__"}
BINARY_SUFFIXES = {".bin", ".jpg", ".png", ".pyc", ".zip"}
QUAKE_DATA_NAME = re.compile(r"pak[0-9]+\.part\.[0-9]+", re.IGNORECASE)
MARKDOWN_REFERENCE = re.compile(r"^\s*\[[^]]+\]:\s*(?:<([^>]+)>|(\S+))")
MARKDOWN_HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$")
PACKAGE_EMBEDDED_C_MARKER = "fplinux-check: package-embedded"
APORT_ROOT = ("alpine", "aports")
APORT_C_SUFFIXES = frozenset({".c"})
APORT_C_FORMAT_SUFFIXES = frozenset({".c", ".h"})
SOURCE_SCOPES = (
    "source",
    "container",
    "metadata",
    "docs",
    "spelling",
    "secrets",
    "licenses",
    "python",
    "shell",
    "alpine",
    "c",
)
_CHECK_COMMAND_TIMEOUT = 15 * 60
_PACKAGE_CONFIG_TIMEOUT = 30
_PYTHON_TEST_TIERS = (
    ("small", 90),
    ("host_process", 180),
    ("host_tool", 240),
    ("artifact", 300),
    ("public_workflow", 90),
)


def fail(message: str) -> NoReturn:
    raise SystemExit(f"check failed: {message}")


def _run_direct(
    command: list[str],
    *,
    timeout: float,
    capture: bool,
) -> subprocess.CompletedProcess[str]:
    """Run one standalone checker tool with a finite process-group lifetime."""
    display = shlex.join(command)
    print("+", display, flush=True)
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        fail(f"command timed out after {timeout:g}s: {display}")
    except BaseException:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, stdout or "", stderr or "")


def run(command: list[str], *, timeout: float = _CHECK_COMMAND_TIMEOUT) -> None:
    """Run one checker tool, using the active stage when logging is available."""
    stage = current_stage()
    if stage is not None:
        stage.run(command, cwd=ROOT, timeout=timeout)
        return
    result = _run_direct(command, timeout=timeout, capture=False)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command)


def capture(command: list[str], *, timeout: float) -> subprocess.CompletedProcess[str]:
    """Capture one checker tool while keeping the same timeout policy as stages."""
    stage = current_stage()
    if stage is None:
        return _run_direct(command, timeout=timeout, capture=True)
    result = stage.capture(command, cwd=ROOT, timeout=timeout)
    return subprocess.CompletedProcess(
        result.args,
        result.returncode,
        result.stdout.decode(errors="replace"),
        result.stderr.decode(errors="replace"),
    )


@contextmanager
def report_stage(reporter: RunReporter | None, name: str) -> Iterator[None]:
    """Use a persistent stage log when the host supplied one."""
    if reporter is None:
        yield
        return
    with reporter.stage(name):
        yield


def source_files(*, enforce_policy: bool) -> list[Path]:
    files: list[Path] = []
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        if path.name == ".fplinux-workspace":
            continue
        if path.is_symlink():
            if enforce_policy:
                fail(f"source symlink is not allowed: {relative}")
            continue
        if not path.is_file():
            continue
        if path.suffix.lower() == ".pak" or QUAKE_DATA_NAME.fullmatch(path.name):
            if enforce_policy:
                fail(f"Quake game data is not allowed in source: {relative}")
            continue
        if path.suffix in BINARY_SUFFIXES:
            if enforce_policy:
                fail(f"binary artifact is not allowed in source: {relative}")
            continue
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


def markdown_anchors(path: Path) -> set[str]:
    """Return GitHub-style anchors declared by one Markdown document."""
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    fenced = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = MARKDOWN_HEADING.fullmatch(line)
        if match is None:
            continue
        heading = re.sub(r"`([^`]*)`", r"\1", match.group(2)).lower()
        base = re.sub(r"[^\w\- ]", "", heading).replace(" ", "-")
        count = counts.get(base, 0)
        counts[base] = count + 1
        anchors.add(base if count == 0 else f"{base}-{count}")
    return anchors


def markdown_inline_destinations(line: str) -> list[str]:
    """Extract inline GFM destinations, including balanced parentheses."""
    destinations: list[str] = []
    position = 0
    while (opening := line.find("](", position)) >= 0:
        cursor = opening + 2
        while cursor < len(line) and line[cursor].isspace():
            cursor += 1
        if cursor >= len(line):
            break
        if line[cursor] == "<":
            closing = line.find(">", cursor + 1)
            if closing < 0:
                position = cursor + 1
                continue
            destinations.append(line[cursor + 1 : closing])
            position = closing + 1
            continue

        value: list[str] = []
        depth = 0
        while cursor < len(line):
            character = line[cursor]
            if character == "\\" and cursor + 1 < len(line):
                value.append(line[cursor + 1])
                cursor += 2
                continue
            if character == "(":
                depth += 1
            elif character == ")":
                if depth == 0:
                    break
                depth -= 1
            elif character.isspace() and depth == 0:
                break
            value.append(character)
            cursor += 1
        if value:
            destinations.append("".join(value))
        position = cursor + 1
    return destinations


def markdown_path_uses_symlink(path: Path, root: Path) -> bool:
    """Return whether a lexical path crosses a symlink inside the source tree."""
    current = path
    while current != root and root in current.parents:
        if current.is_symlink():
            return True
        current = current.parent
    return current.is_symlink()


def check_markdown_links(files: list[Path]) -> None:
    """Require every repository-local Markdown path and anchor to resolve."""
    anchor_cache: dict[Path, set[str]] = {}
    for source in files:
        fenced = False
        for number, raw_line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if raw_line.lstrip().startswith(("```", "~~~")):
                fenced = not fenced
                continue
            if fenced:
                continue
            line = re.sub(r"`[^`]*`", "", raw_line)
            destinations = markdown_inline_destinations(line)
            reference = MARKDOWN_REFERENCE.match(line)
            if reference is not None:
                reference_destination = reference.group(1) or reference.group(2)
                if reference_destination is None:
                    fail(
                        f"Markdown reference destination is invalid: "
                        f"{source.relative_to(ROOT)}:{number}"
                    )
                destinations.append(reference_destination)
            for raw_destination in destinations:
                destination = raw_destination.strip("<>")
                if re.match(r"^[a-z][a-z0-9+.-]*:", destination, re.IGNORECASE):
                    continue
                path_text, separator, fragment = destination.partition("#")
                candidate = source if not path_text else source.parent / unquote(path_text)
                root = ROOT.absolute()
                if markdown_path_uses_symlink(candidate, root):
                    fail(
                        f"Markdown link target is a symlink: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                target = candidate.resolve()
                try:
                    target.relative_to(ROOT.resolve())
                except ValueError:
                    fail(
                        f"Markdown link escapes the source tree: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                if not target.is_file():
                    fail(
                        f"Markdown link target is missing: "
                        f"{source.relative_to(ROOT)}:{number}: {destination}"
                    )
                if separator and target.suffix.lower() == ".md":
                    anchor = unquote(fragment).lower()
                    anchors = anchor_cache.setdefault(target, markdown_anchors(target))
                    if anchor not in anchors:
                        fail(
                            f"Markdown link anchor is missing: "
                            f"{source.relative_to(ROOT)}:{number}: {destination}"
                        )


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
        fail("the Containerfile must use one lock-provided FROM ${BASE_IMAGE}")


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
            fail(f"invalid verified qualification SHA256 for target: {name}")


def quality_sources(
    files: list[Path],
) -> tuple[list[str], list[str], list[str], list[str]]:
    python_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".py"]
    markdown_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".md"]
    posix_shell_files: list[str] = []
    bash_files: list[str] = []
    for path in files:
        if path.suffix not in {"", ".initd", ".sh"}:
            continue
        with path.open("rb") as stream:
            raw_first_line = stream.readline()
        try:
            first_line = raw_first_line.decode().strip()
        except UnicodeDecodeError:
            continue
        relative = str(path.relative_to(ROOT))
        if first_line == "#!/usr/bin/env bash":
            bash_files.append(relative)
        elif first_line in {"#!/bin/sh", "#!/usr/bin/env sh", "#!/sbin/openrc-run"}:
            posix_shell_files.append(relative)
    return python_files, markdown_files, posix_shell_files, bash_files


def alpine_apkbuilds(files: list[Path]) -> list[str]:
    """Discover every regular first-party Alpine aport recipe."""
    result = [
        path.relative_to(ROOT).as_posix()
        for path in files
        if (relative := path.relative_to(ROOT)).parts[:2] == APORT_ROOT
        and len(relative.parts) == 4
        and relative.name == "APKBUILD"
    ]
    if not result:
        fail("no Alpine APKBUILD files were discovered")
    return result


def validate_package_selections() -> None:
    """Require every target's declared package set to resolve to current aports."""
    for target in discover_targets():
        target_config = load_target(target)
        platform = load_platform(target_config["platform"])
        rootfs_packages = alpine_state.selected_packages(platform, target_config, root=ROOT)
        alpine_state.bundle_packages(platform, target_config, rootfs_packages, root=ROOT)


def package_c_is_embedded(path: Path) -> bool:
    """Return whether a package C file is compiled only inside an upstream tree."""
    with path.open(encoding="utf-8") as stream:
        return any(PACKAGE_EMBEDDED_C_MARKER in stream.readline() for _ in range(4))


def is_aport_source(path: Path, suffixes: frozenset[str]) -> bool:
    """Return whether a source belongs to any first-party Alpine aport."""
    relative = path.relative_to(ROOT)
    return relative.parts[:2] == APORT_ROOT and relative.suffix in suffixes


def is_shared_aport_source(path: Path, suffixes: frozenset[str]) -> bool:
    """Return whether a source is one canonical C/H file shared by aports."""
    relative = path.relative_to(ROOT)
    return (
        relative.as_posix() in alpine_state.SHARED_APORT_SOURCE_PATHS
        and relative.suffix in suffixes
    )


def userspace_c_sources(
    files: list[Path], *, include_embedded: bool = False
) -> list[tuple[str, bool]]:
    """Discover userspace C and whether each source needs libusb."""
    result: dict[str, bool] = {}
    for path in files:
        if (
            is_aport_source(path, APORT_C_SUFFIXES)
            or is_shared_aport_source(path, APORT_C_SUFFIXES)
        ) and (include_embedded or not package_c_is_embedded(path)):
            result[path.relative_to(ROOT).as_posix()] = False

    platform_names = {load_target(target)["platform"] for target in discover_targets()}
    for platform_name in sorted(platform_names):
        platform = load_platform(platform_name)
        for recipe in platform["host"]["tools"]:
            if recipe["type"] != "cc-libusb":
                continue
            source = recipe["source"]
            path = ROOT / source
            if path.suffix != ".c" or path.is_symlink() or not path.is_file():
                fail(f"cc-libusb source must be a regular C file: {source}")
            result[source] = True

    if not result:
        fail("no userspace C sources were discovered")
    return sorted(result.items())


def project_c_format_sources(files: list[Path]) -> list[str]:
    """Discover formatted C/H outside the separately checked Linux tree."""
    sources = {
        path.relative_to(ROOT).as_posix()
        for path in files
        if (
            is_aport_source(path, APORT_C_FORMAT_SUFFIXES)
            or is_shared_aport_source(path, APORT_C_FORMAT_SUFFIXES)
            or (
                path.relative_to(ROOT).parts[0] == "tests"
                and path.suffix in APORT_C_FORMAT_SUFFIXES
            )
            or (
                len(path.relative_to(ROOT).parts) >= 4
                and path.relative_to(ROOT).parts[0] in {"platforms", "targets"}
                and path.relative_to(ROOT).parts[2] == "common"
                and path.suffix in APORT_C_FORMAT_SUFFIXES
            )
            or (
                len(path.relative_to(ROOT).parts) >= 6
                and path.relative_to(ROOT).parts[0] == "targets"
                and path.relative_to(ROOT).parts[2] == "profiles"
                and path.relative_to(ROOT).parts[4] == "uboot"
                and path.suffix in APORT_C_FORMAT_SUFFIXES
            )
        )
    }
    sources.update(
        source for source, _requires_libusb in userspace_c_sources(files, include_embedded=True)
    )
    return sorted(sources)


def userspace_c_include_flags(source: str) -> list[str]:
    """Return compile flags needed by one source's project-owned headers."""
    path = PurePosixPath(source)
    if (
        len(path.parts) >= 3
        and path.parts[:2] == APORT_ROOT
        and path.parts[2] in alpine_state.SHARED_APORT_SOURCES
    ):
        return ["-I", "alpine/shared"]
    return []


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
        pkg_config = capture(
            ["pkg-config", "--cflags", "libusb-1.0"],
            timeout=_PACKAGE_CONFIG_TIMEOUT,
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
        flags = [*common_flags, *userspace_c_include_flags(source)]
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
    parser = argparse.ArgumentParser()
    parser.add_argument("scopes", nargs="*", choices=SOURCE_SCOPES)
    args = parser.parse_args()
    selected = tuple(dict.fromkeys(args.scopes)) if args.scopes else SOURCE_SCOPES
    reporter = RunReporter.from_environment("check", "quality")

    with report_stage(reporter, "source-inventory"):
        files = source_files(enforce_policy="source" in selected)
        python_files, markdown_files, posix_shell_files, bash_files = quality_sources(files)
        toml_files = [str(path.relative_to(ROOT)) for path in files if path.suffix == ".toml"]
        json_files = [
            str(path.relative_to(ROOT))
            for path in files
            if path.suffix in {".json", ".jsonc"} and path.name != "package-lock.json"
        ]

    if "source" in selected:
        with report_stage(reporter, "source-text"):
            check_text(files)
    if "container" in selected:
        with report_stage(reporter, "container-policy"):
            check_container_policy(files)
    if "metadata" in selected:
        with report_stage(reporter, "metadata"):
            check_release_lock()
            run(["taplo", "check", *toml_files])
            run(["taplo", "fmt", "--check", *toml_files])
    if "metadata" in selected or "docs" in selected:
        prettier_files = [
            *(markdown_files if "docs" in selected else []),
            *(json_files if "metadata" in selected else []),
        ]
        with report_stage(reporter, "prettier"):
            run(["prettier", "--check", "--ignore-unknown", *prettier_files])
    if "docs" in selected:
        markdown_paths = [path for path in files if path.suffix == ".md"]
        text_files = [
            str(path.relative_to(ROOT))
            for path in files
            if path.suffix == ".txt" and path.relative_to(ROOT).parts[0] != "LICENSES"
        ]
        with report_stage(reporter, "documentation"):
            check_markdown_links(markdown_paths)
            run(["markdownlint-cli2"])
            run(["vale", "--config", ".vale.ini", *markdown_files, *text_files])
    if "spelling" in selected:
        with report_stage(reporter, "spelling"):
            run(["typos", "."])
    if "secrets" in selected:
        with report_stage(reporter, "secrets"):
            run(
                [
                    "gitleaks",
                    "detect",
                    "--no-banner",
                    "--no-git",
                    "--source",
                    ".",
                    "--redact",
                    "--exit-code",
                    "1",
                ]
            )
    if "licenses" in selected:
        with report_stage(reporter, "licenses"):
            run(["reuse", "lint"])
    if "python" in selected:
        with report_stage(reporter, "python"):
            run(["ruff", "check", *python_files])
            run(["ruff", "format", "--check", *python_files])
            run(["mypy", *python_files])
            if (ROOT / "tests").is_dir():
                for tier, timeout in _PYTHON_TEST_TIERS:
                    for interpreter in ("python3", "python3.11"):
                        run(
                            [
                                interpreter,
                                "-m",
                                "unittest",
                                "discover",
                                "-s",
                                f"tests/{tier}",
                                "-t",
                                ".",
                            ],
                            timeout=timeout,
                        )
    if "shell" in selected:
        with report_stage(reporter, "shell"):
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
    if "container" in selected:
        with report_stage(reporter, "container-lint"):
            # Podman's OCI output has no SHELL support, so pipefail cannot be enabled
            # (DL4006); every pipe feeds printf output into a checked sha256sum.
            run(["hadolint", "--ignore", "DL4006", "Containerfile"])
    if "alpine" in selected:
        apkbuilds = alpine_apkbuilds(files)
        with report_stage(reporter, "alpine"):
            alpine_state.load_alpine_lock()
            validate_package_selections()
            run(["sh", "-n", "alpine/abuild.conf"])
            for apkbuild in apkbuilds:
                run(["apkbuild-lint", apkbuild])
    if "c" in selected:
        c_sources = userspace_c_sources(files)
        c_format_sources = project_c_format_sources(files)
        bootstrap_c = [
            str(path.relative_to(ROOT))
            for path in files
            if path.suffix in {".c", ".h"} and "bootstrap" in path.relative_to(ROOT).parts
        ]
        with report_stage(reporter, "c-format"):
            run(
                [
                    "clang-format",
                    "--style=file",
                    "--dry-run",
                    "--Werror",
                    *c_format_sources,
                    *bootstrap_c,
                ]
            )
        with report_stage(reporter, "c-analysis"):
            check_userspace_c(c_sources)

    inventory = (
        f"{len(files)} UTF-8 source files"
        if "source" in selected
        else f"{len(files)} source files inventoried"
    )
    print(f"source checks: OK ({inventory}; scopes: {','.join(selected)})")


if __name__ == "__main__":
    run_entrypoint(main)
