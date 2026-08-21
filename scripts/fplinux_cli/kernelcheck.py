# SPDX-License-Identifier: GPL-2.0-only
"""Prepare real Linux contexts and run sparse on projected kernel C."""

from __future__ import annotations

import argparse
import difflib
import re
import shlex
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import linux_state
from .builder import (
    CACHE,
    prepare_linux,
    report_stage,
    require_file,
    root_source,
    run,
    target_source,
)
from .common import ROOT
from .config import (
    discover_targets,
    load_platform,
    load_target,
    relative_value,
)
from .output import RunReporter, current_stage, exit_status, run_entrypoint

if TYPE_CHECKING:
    from .linux_state import PreparedLinuxState


def load_sources() -> dict[str, Any]:
    """Load the pinned source lock used by the shared Linux preparer."""
    with (ROOT / "sources.lock.toml").open("rb") as stream:
        return tomllib.load(stream)


def patch_c_destinations(path: Path) -> list[str]:
    """Return C paths touched by one validated ``patch -p1`` input."""
    result: list[str] = []
    old_remaining = 0
    new_remaining = 0
    expect_destination = False
    hunk = re.compile(r"^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")
    for line in require_file(path).read_text().splitlines():
        if old_remaining or new_remaining:
            if line.startswith("\\"):
                continue
            marker = line[:1] or " "
            if marker in {" ", "-"}:
                old_remaining -= 1
            if marker in {" ", "+"}:
                new_remaining -= 1
            if marker not in {" ", "-", "+"} or min(old_remaining, new_remaining) < 0:
                raise SystemExit(f"sparse failed: malformed patch hunk: {path}")
            continue

        match = hunk.match(line)
        if match is not None:
            old_remaining = int(match.group(1) or "1")
            new_remaining = int(match.group(2) or "1")
            expect_destination = False
            continue
        if expect_destination:
            expect_destination = False
            if not line.startswith("+++ "):
                continue
            patched = line[4:].split("\t", 1)[0]
            if patched == "/dev/null":
                continue
            if patched.startswith("/"):
                raise SystemExit(f"sparse failed: absolute patch destination: {path}")
            prefix, separator, destination = patched.partition("/")
            if not separator or prefix in {"", ".."}:
                raise SystemExit(f"sparse failed: patch destination has no -p1 prefix: {path}")
            destination = relative_value(destination, "Linux patch C destination")
            if destination.endswith(".c"):
                result.append(destination)
            continue
        if line.startswith("--- "):
            expect_destination = True
    if old_remaining or new_remaining:
        raise SystemExit(f"sparse failed: incomplete patch hunk: {path}")
    return result


def sparse_targets(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> list[str]:
    """Resolve every project-owned or project-patched Linux C object."""
    linux = platform["linux"]
    destinations: list[str] = []
    for relative in linux["patches"]:
        destinations.extend(patch_c_destinations(root_source(relative)))
    for relative in target_config["linux"]["patches"]:
        destinations.extend(patch_c_destinations(target_source(target, relative)))
    for step in [
        *linux["copies"],
        *target_config["linux"]["copies"],
        *linux["appends"],
        *target_config["linux"]["appends"],
    ]:
        destination = step["destination"]
        if destination.endswith(".c"):
            destinations.append(destination)

    objects = list(dict.fromkeys(str(Path(path).with_suffix(".o")) for path in destinations))
    if not objects:
        raise SystemExit(f"sparse failed: target has no projected kernel C: {target}")
    return objects


def projected_sources(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> list[Path]:
    """Collect project sources that are projected into the Linux tree."""
    linux = platform["linux"]
    result = [root_source(step["source"]) for step in [*linux["copies"], *linux["appends"]]]
    result.extend(
        target_source(target, step["source"])
        for step in [*target_config["linux"]["copies"], *target_config["linux"]["appends"]]
    )
    if not result:
        raise SystemExit(f"sparse failed: target projects no kernel sources: {target}")
    return result


def record_text(text: str) -> None:
    """Append captured diagnostics to the active stage or the terminal."""
    stage = current_stage()
    if stage is None:
        print(text, end="")
        return
    stage.write(text.encode())


def capture_text(command: list[str]) -> subprocess.CompletedProcess[str]:
    """Capture a command for policy inspection while retaining reporter output."""
    stage = current_stage()
    if stage is None:
        print("+", shlex.join(command), flush=True)
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result
    captured = stage.capture(command)
    return subprocess.CompletedProcess(
        captured.args,
        captured.returncode,
        captured.stdout.decode(errors="replace"),
        captured.stderr.decode(errors="replace"),
    )


def run_checkpatch(command: list[str]) -> None:
    """Run one checkpatch pass and fail on any reported finding."""
    report = capture_text(command)
    if report.returncode:
        record_text(f"checkpatch exited {report.returncode}\n")
        raise SystemExit(exit_status(report.returncode))
    if "WARNING:" in report.stdout or "ERROR:" in report.stdout:
        message = "sparse failed: checkpatch reported findings"
        raise SystemExit(message)


def run_dtbs_check(command: list[str], target: str) -> str:
    """Run dtbs_check and return its combined diagnostic output."""
    report = capture_text(command)
    if report.returncode:
        record_text(f"dtbs_check exited {report.returncode}: {target}\n")
        raise SystemExit(exit_status(report.returncode))
    return report.stdout + report.stderr


def sparse_cache_directory(target: str) -> Path:
    """Return the fixed Sparse output directory for one target."""
    return CACHE / "analysis" / "sparse" / target


def sparse_output(target: str) -> Path:
    """Return the one fixed Kbuild output path for one target."""
    return sparse_cache_directory(target) / "work"


def reset_sparse_output(target: str) -> Path:
    """Cold-reset the one generated Kbuild output before each analysis."""
    output = sparse_output(target)
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    return output


def target_context(
    sources: dict[str, Any], target: str
) -> tuple[dict[str, Any], dict[str, Any], Path, PreparedLinuxState]:
    """Load one target and prepare its exact pinned Linux integration tree."""
    target_config = load_target(target)
    platform = load_platform(target_config["platform"])
    source, prepared_linux = prepare_linux(sources, target, target_config, platform)
    return target_config, platform, source, prepared_linux


def prepare_contexts(reporter: RunReporter | None) -> None:
    """Populate each target's current recipe-validated Linux source context."""
    sources = load_sources()
    for target in discover_targets():
        with report_stage(reporter, f"prepare-{target}"):
            _config, _platform, _source, prepared_linux = target_context(sources, target)
            record_text(f"sparse context: ready ({target}, {prepared_linux.linux_recipe[:16]})\n")


def check_contexts(reporter: RunReporter | None) -> None:
    """Run sparse through Kbuild with target Kconfig and generated headers."""
    sources = load_sources()
    checked = 0
    for target in discover_targets():
        with report_stage(reporter, f"context-{target}"):
            target_config, platform, source, prepared_linux = target_context(sources, target)
            projected = projected_sources(target, target_config, platform)
            style_files = [str(path) for path in projected if path.suffix in {".c", ".h"}]
            checkpatch = [
                str(require_file(source / "scripts/checkpatch.pl")),
                f"--root={source}",
                "--terse",
            ]
            kconfig_files = [str(path) for path in projected if path.name == "Kconfig"]
            patch_files = [
                str(root_source(relative)) for relative in platform["linux"]["patches"]
            ] + [
                str(target_source(target, relative))
                for relative in target_config["linux"]["patches"]
            ]
            defconfig = require_file(target_source(target, target_config["linux"]["defconfig"]))
            objects = sparse_targets(target, target_config, platform)
            output = sparse_output(target)
            kbuild = [
                "make",
                "-C",
                str(source),
                f"O={output}",
                f"ARCH={platform['linux']['arch']}",
                f"CROSS_COMPILE={platform['linux']['analysis_cross_compile']}",
            ]
            format_command = [
                "clang-format",
                f"--style=file:{require_file(source / '.clang-format')}",
                "--dry-run",
                "--Werror",
                *style_files,
            ]
            checkpatch_sources = [*checkpatch, "-f", *style_files, *kconfig_files]
            checkpatch_patches = [*checkpatch, *patch_files] if patch_files else None
            kconfig_command = [*kbuild, "olddefconfig", "prepare"]
            save_defconfig_command = [*kbuild, "savedefconfig"]
            dtbs_command = [*kbuild, "W=1", "dtbs_check"]
            sparse_command = [
                *kbuild,
                "-j1",
                "W=1e",
                "C=2",
                "CHECK=sparse",
                "CF=-D__CHECK_ENDIAN__ -Wsparse-error",
                *objects,
            ]
            linux_state.require_prepared_linux(source, prepared_linux)

        output = reset_sparse_output(target)
        with report_stage(reporter, f"format-{target}"):
            run(format_command)
        with report_stage(reporter, f"checkpatch-{target}"):
            # --root resolves the fplinux compatibles against projected bindings.
            run_checkpatch(checkpatch_sources)
            if checkpatch_patches is not None:
                run_checkpatch(checkpatch_patches)
        with report_stage(reporter, f"kconfig-{target}"):
            shutil.copyfile(defconfig, output / ".config")
            run(kconfig_command)
            run(save_defconfig_command)
            current = defconfig.read_text()
            canonical = require_file(output / "defconfig").read_text()
            if canonical != current:
                record_text(
                    "".join(
                        difflib.unified_diff(
                            current.splitlines(keepends=True),
                            canonical.splitlines(keepends=True),
                            fromfile=str(defconfig),
                            tofile="savedefconfig",
                        )
                    )
                )
                raise SystemExit(f"sparse failed: defconfig is not canonical: {defconfig}")
        with report_stage(reporter, f"device-tree-{target}"):
            combined = run_dtbs_check(dtbs_command, target)
            if "Warning" in combined or re.search(r"\.dtb: ", combined):
                raise SystemExit(f"sparse failed: device tree findings: {target}")
        with report_stage(reporter, f"sparse-{target}"):
            run(sparse_command)
        checked += len(objects)
        print(f"sparse: OK ({target}, {len(objects)} kernel C objects)")
    print(f"sparse: OK ({checked} kernel C objects total)")


def main() -> None:
    """Dispatch the internal preparation or offline analysis phase."""
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "check"))
    args = parser.parse_args()
    reporter = RunReporter.from_environment("check", f"kernel-{args.phase}")
    if args.phase == "prepare":
        prepare_contexts(reporter)
    else:
        check_contexts(reporter)


if __name__ == "__main__":
    run_entrypoint(main)
