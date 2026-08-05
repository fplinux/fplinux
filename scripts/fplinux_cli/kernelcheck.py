# SPDX-License-Identifier: GPL-2.0-only
"""Prepare real Linux contexts and run sparse on projected kernel C."""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import re
import shlex
import shutil
import subprocess
import tomllib
from pathlib import Path
from typing import Any

from .builder import (
    CACHE,
    prepare_linux,
    require_file,
    root_source,
    run,
    target_source,
)
from .common import ROOT, sha256_file
from .config import (
    discover_targets,
    load_platform,
    load_target,
    relative_value,
    toolchain_recipe_digest,
)


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


def run_checkpatch(command: list[str]) -> None:
    """Run one checkpatch pass and fail on any reported finding."""
    print("+", shlex.join(command), flush=True)
    report = subprocess.run(command, capture_output=True, text=True, check=False)
    print(report.stdout, end="")
    print(report.stderr, end="")
    if report.returncode or "WARNING:" in report.stdout or "ERROR:" in report.stdout:
        message = "sparse failed: checkpatch reported findings"
        raise SystemExit(message)


def sparse_recipe_digest(
    platform: dict[str, Any], linux_recipe: str, defconfig: Path, objects: list[str]
) -> str:
    """Hash every input that can change the generated sparse Kbuild context."""
    manifest = {
        "schema": "fplinux.sparse/v1",
        "toolchain_recipe": toolchain_recipe_digest(),
        "checker_sha256": sha256_file(Path(__file__)),
        "builder_sha256": sha256_file(Path(__file__).with_name("builder.py")),
        "linux_recipe": linux_recipe,
        "defconfig_sha256": sha256_file(defconfig),
        "arch": platform["linux"]["arch"],
        "cross_compile": platform["linux"]["analysis_cross_compile"],
        "objects": objects,
        "make_flags": ["C=2", "CHECK=sparse", "-D__CHECK_ENDIAN__", "-Wsparse-error"],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def target_context(
    sources: dict[str, Any], target: str
) -> tuple[dict[str, Any], dict[str, Any], Path, str]:
    """Load one target and prepare its exact pinned Linux integration tree."""
    target_config = load_target(target)
    platform = load_platform(target_config["platform"])
    source, recipe = prepare_linux(sources, target, target_config, platform)
    return target_config, platform, source, recipe


def prepare_contexts() -> None:
    """Populate each target's current recipe-validated Linux source context."""
    sources = load_sources()
    for target in discover_targets():
        _config, _platform, _source, recipe = target_context(sources, target)
        print(f"sparse context: ready ({target}, {recipe[:16]})")


def check_contexts() -> None:
    """Run sparse through Kbuild with target Kconfig and generated headers."""
    sources = load_sources()
    checked = 0
    for target in discover_targets():
        target_config, platform, source, recipe = target_context(sources, target)
        projected = projected_sources(target, target_config, platform)
        style_files = [str(path) for path in projected if path.suffix in {".c", ".h"}]
        run(
            [
                "clang-format",
                f"--style=file:{require_file(source / '.clang-format')}",
                "--dry-run",
                "--Werror",
                *style_files,
            ]
        )
        # --root lets checkpatch resolve the fplinux compatibles against the
        # projected DT bindings and vendor prefix in the prepared tree.
        checkpatch = [
            str(require_file(source / "scripts/checkpatch.pl")),
            f"--root={source}",
            "--terse",
        ]
        kconfig_files = [str(path) for path in projected if path.name == "Kconfig"]
        run_checkpatch([*checkpatch, "-f", *style_files, *kconfig_files])
        patch_files = [str(root_source(relative)) for relative in platform["linux"]["patches"]] + [
            str(target_source(target, relative)) for relative in target_config["linux"]["patches"]
        ]
        if patch_files:
            run_checkpatch([*checkpatch, *patch_files])
        defconfig = require_file(target_source(target, target_config["linux"]["defconfig"]))
        objects = sparse_targets(target, target_config, platform)
        analysis_recipe = sparse_recipe_digest(platform, recipe, defconfig, objects)
        output = CACHE / "analysis/sparse" / target / analysis_recipe
        if output.is_symlink() or (output.exists() and not output.is_dir()):
            raise SystemExit(f"sparse failed: invalid output path: {output}")
        output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(defconfig, output / ".config")

        kbuild = [
            "make",
            "-C",
            str(source),
            f"O={output}",
            f"ARCH={platform['linux']['arch']}",
            f"CROSS_COMPILE={platform['linux']['analysis_cross_compile']}",
        ]
        run([*kbuild, "olddefconfig", "prepare"])
        run([*kbuild, "savedefconfig"])
        current = defconfig.read_text()
        canonical = require_file(output / "defconfig").read_text()
        if canonical != current:
            print(
                "".join(
                    difflib.unified_diff(
                        current.splitlines(keepends=True),
                        canonical.splitlines(keepends=True),
                        fromfile=str(defconfig),
                        tofile="savedefconfig",
                    )
                ),
                end="",
            )
            raise SystemExit(f"sparse failed: defconfig is not canonical: {defconfig}")
        dtbs_command = [*kbuild, "W=1", "dtbs_check"]
        print("+", shlex.join(dtbs_command), flush=True)
        dtbs = subprocess.run(dtbs_command, capture_output=True, text=True, check=True)
        print(dtbs.stdout, end="")
        print(dtbs.stderr, end="")
        combined = dtbs.stdout + dtbs.stderr
        if "Warning" in combined or re.search(r"\.dtb: ", combined):
            raise SystemExit(f"sparse failed: device tree findings: {target}")
        for object_path in objects:
            run(
                [
                    *kbuild,
                    "C=2",
                    "CHECK=sparse",
                    "CF=-D__CHECK_ENDIAN__ -Wsparse-error",
                    object_path,
                ]
            )
        checked += len(objects)
        print(f"sparse: OK ({target}, {len(objects)} kernel C objects)")
    print(f"sparse: OK ({checked} kernel C objects total)")


def main() -> None:
    """Dispatch the internal preparation or offline analysis phase."""
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "check"))
    args = parser.parse_args()
    if args.phase == "prepare":
        prepare_contexts()
    else:
        check_contexts()


if __name__ == "__main__":
    main()
