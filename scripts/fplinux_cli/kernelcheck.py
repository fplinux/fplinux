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
    profile_kconfig_actions,
    profile_kconfig_arguments,
    report_stage,
    require_file,
    root_source,
    run,
    target_source,
)
from .common import ROOT
from .config import (
    discover_profiles,
    discover_targets,
    load_platform,
    load_target,
    relative_value,
    target_defconfig_path,
)
from .device_tree import DeviceTreeError, verify_target_identity
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


def assert_profile_kconfig(
    config: Path, config_enable: list[str], config_disable: list[str]
) -> None:
    """Require profile Kconfig actions to survive dependency resolution."""
    text = require_file(config).read_text()
    for symbol in config_enable:
        if f"{symbol}=y\n" not in text:
            raise SystemExit(f"sparse failed: profile did not enable {symbol}")
    for symbol in config_disable:
        if f"# {symbol} is not set\n" not in text:
            raise SystemExit(f"sparse failed: profile did not disable {symbol}")


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


def target_profiles(profile: str | None = None) -> tuple[tuple[str, str | None], ...]:
    """Select every default context, or one explicitly named profile."""
    if profile is None:
        return tuple((target, None) for target in discover_targets())

    selected: list[tuple[str, str | None]] = []
    for target in discover_targets():
        profiles = discover_profiles(target)
        if profile in profiles:
            selected.append((target, profile))
    if profile is not None and not selected:
        raise SystemExit(f"sparse failed: profile is not declared by any target: {profile}")
    return tuple(selected)


def context_label(target: str, profile: str | None) -> str:
    """Return the stable stage label for one target/profile kernel context."""
    return target if profile is None else f"{target}-profile-{profile}"


def sparse_cache_directory(target: str, profile: str | None = None) -> Path:
    """Return the fixed Sparse output directory for one target/profile."""
    root = CACHE / "analysis" / "sparse" / target
    return root if profile is None else root / "profiles" / profile


def sparse_output(target: str, profile: str | None = None) -> Path:
    """Return the one fixed Kbuild output path for one target/profile."""
    return sparse_cache_directory(target, profile) / "work"


def reset_sparse_output(target: str, profile: str | None = None) -> Path:
    """Cold-reset the one generated Kbuild output before each analysis."""
    output = sparse_output(target, profile)
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    return output


def target_context(
    sources: dict[str, Any], target: str, profile: str | None = None
) -> tuple[dict[str, Any], dict[str, Any], Path, PreparedLinuxState]:
    """Load one target/profile and prepare its exact Linux integration tree."""
    target_config = load_target(target, profile)
    platform = load_platform(target_config["platform"])
    source, prepared_linux = prepare_linux(sources, target, target_config, platform)
    return target_config, platform, source, prepared_linux


def prepare_contexts(reporter: RunReporter | None, profile: str | None = None) -> None:
    """Populate default contexts or one explicitly selected profile."""
    sources = load_sources()
    for target, selected in target_profiles(profile):
        label = context_label(target, selected)
        with report_stage(reporter, f"prepare-{label}"):
            _config, _platform, _source, prepared_linux = target_context(sources, target, selected)
            record_text(f"sparse context: ready ({label}, {prepared_linux.linux_recipe[:16]})\n")


def check_contexts(reporter: RunReporter | None, profile: str | None = None) -> None:
    """Run sparse through Kbuild for default or explicitly selected contexts."""
    sources = load_sources()
    checked = 0
    for target, selected in target_profiles(profile):
        label = context_label(target, selected)
        with report_stage(reporter, f"context-{label}"):
            target_config, platform, source, prepared_linux = target_context(
                sources, target, selected
            )
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
            defconfig = require_file(target_defconfig_path(target))
            objects = sparse_targets(target, target_config, platform)
            output = sparse_output(target, selected)
            config_enable, config_disable = profile_kconfig_actions(target_config)
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
            first_kconfig_command = [*kbuild, "olddefconfig"]
            profile_config_command: list[str] | None = None
            if config_enable or config_disable:
                profile_config_command = [
                    str(require_file(source / platform["linux"]["config_script"])),
                    "--file",
                    str(output / ".config"),
                    *profile_kconfig_arguments(config_enable, config_disable),
                ]
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

        output = reset_sparse_output(target, selected)
        with report_stage(reporter, f"format-{label}"):
            run(format_command)
        with report_stage(reporter, f"checkpatch-{label}"):
            # --root resolves the fplinux compatibles against projected bindings.
            run_checkpatch(checkpatch_sources)
            if checkpatch_patches is not None:
                run_checkpatch(checkpatch_patches)
        with report_stage(reporter, f"kconfig-{label}"):
            shutil.copyfile(defconfig, output / ".config")
            if profile_config_command is not None:
                run(first_kconfig_command)
                run(profile_config_command)
            run(kconfig_command)
            if selected is None:
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
            else:
                assert_profile_kconfig(output / ".config", config_enable, config_disable)
        with report_stage(reporter, f"device-tree-{label}"):
            combined = run_dtbs_check(dtbs_command, target)
            if "Warning" in combined or re.search(r"\.dtb: ", combined):
                raise SystemExit(f"sparse failed: device tree findings: {target}")
            identity = target_config["identity"]
            platform_identity = platform["identity"]
            dtb = (
                output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
            )
            try:
                verify_target_identity(
                    dtb,
                    target,
                    identity["display_name"],
                    (identity["compatible"], platform_identity["compatible"]),
                )
            except DeviceTreeError as error:
                raise SystemExit(f"sparse failed: {error}") from error
        with report_stage(reporter, f"sparse-{label}"):
            run(sparse_command)
        checked += len(objects)
        print(f"sparse: OK ({label}, {len(objects)} kernel C objects)")
    print(f"sparse: OK ({checked} kernel C objects total)")


def main() -> None:
    """Dispatch the internal preparation or offline analysis phase."""
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "check"))
    parser.add_argument("--profile")
    args = parser.parse_args()
    reporter = RunReporter.from_environment("check", f"kernel-{args.phase}")
    if args.phase == "prepare":
        prepare_contexts(reporter, args.profile)
    else:
        check_contexts(reporter, args.profile)


if __name__ == "__main__":
    run_entrypoint(main)
