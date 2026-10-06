# SPDX-License-Identifier: GPL-2.0-only
"""Analyze projected kernel sources, schemas, configuration and objects."""

from __future__ import annotations

import os
import re
import shlex
import signal
import subprocess
import sys
from contextlib import suppress
from pathlib import Path
from typing import Any

from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
)
from fplinux_cli.build.inputs import CACHE, require_file, root_source, target_source
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.build.kernel.configuration import (
    profile_kconfig_actions,
    profile_kconfig_arguments,
)
from fplinux_cli.build.kernel.projection import patch_destinations
from fplinux_cli.build.process import report_stage, run
from fplinux_cli.build.sources import source_lock_entry
from fplinux_cli.common import fail
from fplinux_cli.manifests.kernel import compose_kernel_config, kconfig_values, kernel_config_paths
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.quality.kernel_patches import binding_paths, check_linux_changes, context_inputs
from fplinux_cli.reporting.process import exit_status
from fplinux_cli.reporting.run import RunReporter, current_stage

from .contexts import context_label, reset_sparse_output, sparse_output, target_context

_KERNEL_CAPTURE_TIMEOUT = 30 * 60


def patch_c_destinations(path: Path) -> list[str]:
    """Select compilable C destinations from the shared Linux patch reader."""
    return [
        name for name in patch_destinations(path, include_deleted=False) if name.endswith(".c")
    ]


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
        fail(f"kernel check failed: target has no projected kernel C: {target}")
    return objects


def sparse_build_targets(
    source: Path,
    output: Path,
    objects: list[str],
    *,
    arch: str,
    cross_compile: str,
) -> list[str]:
    """Ask Kbuild which projected objects belong to the resolved configuration."""
    selected: set[str] = set()
    for directory in dict.fromkeys(str(Path(path).parent) for path in objects):
        result = capture_text(
            [
                "make",
                "--no-print-directory",
                "-s",
                "-C",
                str(output),
                "-f",
                str(require_file(source / "scripts/Makefile.build")),
                f"srctree={source}",
                f"srcroot={source}",
                f"objtree={output}",
                f"obj={directory}",
                f"ARCH={arch}",
                f"SRCARCH={arch}",
                f"CROSS_COMPILE={cross_compile}",
                f"CC={cross_compile}gcc",
                "need-builtin=1",
                "need-modorder=1",
                "KBUILD_BUILTIN=1",
                "KBUILD_MODULES=1",
                (
                    "--eval=.PHONY: __fplinux_selected\n__fplinux_selected: ; "
                    "@printf '%s\\n' $(filter %.o,$(real-obj-y) $(real-obj-m) $(lib-y))"
                ),
                "__fplinux_selected",
            ]
        )
        if result.returncode:
            fail(f"kernel check failed: object selection exited {exit_status(result.returncode)}")
        selected.update(result.stdout.split())
    return [path for path in objects if path in selected]


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
        fail(f"kernel check failed: target projects no kernel sources: {target}")
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
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(timeout=_KERNEL_CAPTURE_TIMEOUT)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            fail(
                f"kernel check failed: command timed out after {_KERNEL_CAPTURE_TIMEOUT}s: "
                f"{shlex.join(command)}"
            )
        except BaseException:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            raise
        print(stdout, end="")
        print(stderr, end="", file=sys.stderr)
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    captured = stage.capture(command, timeout=_KERNEL_CAPTURE_TIMEOUT)
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
        message = "kernel check failed: checkpatch reported findings"
        fail(message)


def run_dtbs_check(command: list[str], target: str) -> str:
    """Run dtbs_check and return its combined diagnostic output."""
    report = capture_text(command)
    if report.returncode:
        record_text(f"dtbs_check exited {report.returncode}: {target}\n")
        raise SystemExit(exit_status(report.returncode))
    return report.stdout + report.stderr


def check_bindings(
    source: Path, kbuild: list[str], bindings: tuple[str, ...], target: str
) -> None:
    """Check selected schemas and examples without relying on make's masked lint status."""
    schema_root = source / "Documentation/devicetree/bindings"
    paths = [str(source / name) for name in bindings]
    run(["yamllint", "--strict", "-c", str(schema_root / ".yamllint"), *paths])
    report = capture_text(["dt-doc-validate", "-u", str(schema_root), *paths])
    # Reference diagnostics can be printed even when dt-doc-validate returns zero.
    if report.returncode or report.stdout.strip() or report.stderr.strip():
        fail(f"kernel check failed: binding schema findings: {target}")
    combined = run_dtbs_check(
        [*kbuild, "W=1", "dt_binding_check", "DT_SCHEMA_FILES=" + ":".join(bindings)], target
    )
    if re.search(r"(?im)\b(?:warning|error)(?:\s*\([^\n)]*\))?:|\.example\.dtb:", combined):
        fail(f"kernel check failed: binding example findings: {target}")


def check_one_context(
    reporter: RunReporter | None,
    sources: dict[str, Any],
    target: str,
    profile: str | None,
    *,
    build_type: str = "release",
) -> int:
    """Run the complete analyzer sequence for one target-owned context."""
    label = context_label(target, profile)
    with report_stage(reporter, f"context-{label}"):
        target_config, platform, source, prepared_linux = target_context(
            sources, target, profile, build_type=build_type
        )
        projected = projected_sources(target, target_config, platform)
        style_files = [str(path) for path in projected if path.suffix in {".c", ".h"}]
        checkpatch = [
            str(require_file(source / "scripts/checkpatch.pl")),
            f"--root={source}",
            "--terse",
        ]
        kconfig_files = [str(path) for path in projected if path.name == "Kconfig"]
        inputs = context_inputs(target, target_config, platform)
        bindings = binding_paths(inputs, source)
        linux = source_lock_entry(sources, platform["linux"]["source_lock"])
        archive = CACHE / "downloads/linux" / f"linux-{linux['version']}.tar.xz"
        config_paths = kernel_config_paths(target, target_config, platform)
        defconfig = compose_kernel_config(*config_paths).decode()
        objects = sparse_targets(target, target_config, platform)
        output = sparse_output(target, profile, build_type=build_type)
        initramfs = output / "initramfs"
        if target_config["linux"]["root"]["kind"] == "initramfs":
            defconfig += f'CONFIG_INITRAMFS_SOURCE="{initramfs}"\n'
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
        dtbs_command = [*kbuild, "W=1", "dtbs_check"]
        linux_state.require_prepared_linux(source, prepared_linux)

    output = reset_sparse_output(target, profile, build_type=build_type)
    linux_state.write_profile_root(output, target_config)
    config_diff = output / "integration-kbuild.patch"
    with report_stage(reporter, f"format-{label}"):
        run(format_command)
        patch_files = check_linux_changes(inputs, archive, linux, config_diff)
    with report_stage(reporter, f"checkpatch-{label}"):
        # --root resolves the fplinux compatibles against projected bindings.
        run_checkpatch(checkpatch_sources)
        if patch_files:
            run_checkpatch([*checkpatch, *(str(path) for path in patch_files)])
        if config_diff.stat().st_size:
            run_checkpatch([*checkpatch, str(config_diff)])
    with report_stage(reporter, f"kconfig-{label}"):
        if target_config["linux"]["root"]["kind"] == "initramfs":
            initramfs.mkdir()
        (output / ".config").write_text(defconfig)
        if profile_config_command is not None:
            run(first_kconfig_command)
            run(profile_config_command)
        run(kconfig_command)
        requested = kconfig_values(defconfig)
        requested.update(dict.fromkeys(config_enable, "y"))
        requested.update(dict.fromkeys(config_disable, "n"))
        if target_config["linux"]["root"]["kind"] != "initramfs":
            # An external-root kernel has no embedded archive to compress.
            requested = {
                symbol: value
                for symbol, value in requested.items()
                if not symbol.startswith("CONFIG_INITRAMFS_COMPRESSION_")
            }
        actual = kconfig_values(require_file(output / ".config").read_text())
        for symbol, value in requested.items():
            if actual.get(symbol, "n") != value:
                fail(
                    f"kernel check failed: kernel configuration did not preserve {symbol}={value}"
                )
    if bindings:
        with report_stage(reporter, f"bindings-{label}"):
            check_bindings(source, kbuild, bindings, target)
    with report_stage(reporter, f"device-tree-{label}"):
        combined = run_dtbs_check(dtbs_command, target)
        if "Warning" in combined or re.search(r"\.dtb: ", combined):
            fail(f"kernel check failed: device tree findings: {target}")
        identity = target_config["identity"]
        platform_identity = platform["identity"]
        dtb = output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
        try:
            verify_target_identity(
                dtb,
                target,
                identity["display_name"],
                (identity["compatible"], platform_identity["compatible"]),
            )
            verify_root_bootargs(dtb, target_config["linux"]["root"])
            layout = target_config.get("layout")
            if isinstance(layout, dict):
                verify_profile_dtb_layout(dtb, layout, target_config["linux"]["memory"])
        except DeviceTreeError as error:
            fail(f"kernel check failed: {error}")
    with report_stage(reporter, f"sparse-{label}"):
        configured_objects = sparse_build_targets(
            source,
            output,
            objects,
            arch=platform["linux"]["arch"],
            cross_compile=platform["linux"]["analysis_cross_compile"],
        )
        if configured_objects:
            run(
                [
                    *kbuild,
                    "-j1",
                    "W=1e",
                    "C=2",
                    "CHECK=sparse",
                    "CF=-D__CHECK_ENDIAN__ -Wsparse-error",
                    *configured_objects,
                ]
            )
    checked = sum((output / path).is_file() for path in objects)
    print(f"sparse: OK ({label}, {checked} kernel C objects)")
    return checked


def context_object_count(target: str, profile: str | None, *, build_type: str = "release") -> int:
    """Count projected objects actually compiled in the selected analysis context."""
    target_config = load_target(target, profile, build_type=build_type)
    platform = load_platform(target_config["platform"])
    output = sparse_output(target, profile, build_type=build_type)
    return sum(
        (output / path).is_file() for path in sparse_targets(target, target_config, platform)
    )
