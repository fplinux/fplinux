# SPDX-License-Identifier: GPL-2.0-only
"""Prepare compatible Linux integrations in one shared upstream source tree."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.identity import (
    linux_identity_dtsi,
    linux_identity_dtsi_name,
    linux_machine_binding,
    linux_machine_binding_path,
    linux_platform_identity_header,
)
from fplinux_cli.build.storage import layout as profile_layout
from fplinux_cli.common import fail, sha256_bytes, sha256_file
from fplinux_cli.manifests.linux import discover_linux_targets, is_shared_linux_operation

from . import state as linux_state
from .projection import LinuxInput, file_contents, project_changes
from .state import LinuxStateError, PreparedLinuxState

if TYPE_CHECKING:
    from collections.abc import Sequence

    from fplinux_cli.manifests.linux import LinuxTarget


def integration_inputs(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> list[tuple[str, str, str, Path]]:
    """Return typed Linux recipe inputs in projection order."""
    platform_linux = platform["linux"]
    target_linux = target_config["linux"]
    result = [
        (
            "platform-patch",
            relative,
            "",
            inputs_build.require_file(inputs_build.root_source(relative)),
        )
        for relative in platform_linux["patches"]
    ]

    def add_steps(operation: str, steps: list[dict[str, Any]], *, platform_owned: bool) -> None:
        for step in steps:
            relative = step["source"]
            if platform_owned:
                identity = relative
                path = inputs_build.root_source(relative)
            else:
                identity = f"targets/{target}/{relative}"
                path = inputs_build.target_source(target, relative)
            result.append(
                (operation, identity, step["destination"], inputs_build.require_file(path))
            )

    add_steps("platform-copy", platform_linux["copies"], platform_owned=True)
    add_steps("target-copy", target_linux["copies"], platform_owned=False)
    result.extend(
        (
            "target-patch",
            f"targets/{target}/{relative}",
            "",
            inputs_build.require_file(inputs_build.target_source(target, relative)),
        )
        for relative in target_linux["patches"]
    )
    add_steps("platform-append", platform_linux["appends"], platform_owned=True)
    add_steps("target-append", target_linux["appends"], platform_owned=False)
    return result


def generated_linux_files(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, bytes]:
    """Return exact generated Linux files keyed by destination."""
    target_identity = target_config["identity"]
    platform_identity = platform["identity"]
    platform_linux = platform["linux"]
    dts_directory = platform_linux["dts_directory"]
    return {
        f"{dts_directory}/{linux_identity_dtsi_name(target)}": linux_identity_dtsi(
            target_identity, platform_identity
        ),
        platform_linux["platform_identity_header"]: linux_platform_identity_header(
            platform_identity
        ),
        linux_machine_binding_path(
            target_identity, arch=platform_linux["arch"]
        ): linux_machine_binding(target_identity, platform_identity, arch=platform_linux["arch"]),
    }


def linux_recipe_digest(
    linux_source: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> str:
    """Hash the pinned Linux release and every ordered projection operation."""
    version = linux_source.get("version")
    if not isinstance(version, str) or not version:
        fail("Linux source version must be a non-empty string")
    source_digest = inputs_build.require_sha256(linux_source.get("sha256"), "Linux source")
    manifest = {
        "version": version,
        "sha256": source_digest,
        "integration": [
            {
                "operation": operation,
                "source": relative,
                "destination": destination,
                "sha256": sha256_file(path),
            }
            for operation, relative, destination, path in integration_inputs(
                target, target_config, platform
            )
        ],
        "root_bootargs": sha256_bytes(
            profile_layout.root_bootargs_dtsi(target_config["linux"]["root"])
        ),
        "generated": [
            {"destination": destination, "sha256": sha256_bytes(contents)}
            for destination, contents in sorted(
                generated_linux_files(target, target_config, platform).items()
            )
        ],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    return sha256_bytes(encoded)


def shared_integration(
    targets: tuple[LinuxTarget, ...],
) -> tuple[tuple[LinuxInput, ...], dict[str, bytes]]:
    """Merge ordered integrations, rejecting ambiguous file owners."""
    inputs = []
    seen: set[LinuxInput] = set()
    copies: dict[str, LinuxInput] = {}
    generated: dict[str, bytes] = {}
    for target in targets:
        if target.config.get("microsd", {}).get("linux_patches"):
            fail(f"target {target.name}: profile-specific Linux patches cannot share source")
        for values in integration_inputs(target.name, target.config, target.platform):
            step = LinuxInput(*values)
            if step in seen:
                continue
            seen.add(step)
            if step.operation.endswith("copy"):
                previous = copies.get(step.destination)
                if previous is not None:
                    fail(
                        f"Linux copy destination has multiple owners: {step.destination} "
                        f"({previous.identity}, {step.identity})"
                    )
                copies[step.destination] = step
            inputs.append(step)
        files = generated_linux_files(target.name, target.config, target.platform)
        for name, contents in files.items():
            if name in generated and generated[name] != contents:
                fail(f"Linux generated destination has conflicting owners: {name}")
            generated[name] = contents
    # Every platform patch/copy precedes board patches; all append fragments apply once.
    order = {
        "platform-patch": 0,
        "platform-copy": 1,
        "target-copy": 2,
        "target-patch": 3,
        "platform-append": 4,
        "target-append": 5,
    }
    inputs.sort(key=lambda step: order[step.operation])
    return tuple(inputs), generated


def shared_recipe_digest(
    source_sha256: str, inputs: Sequence[LinuxInput], generated: dict[str, bytes]
) -> str:
    """Hash the bounded aggregate projection, independently of a selected build profile."""
    payload = {
        "source_sha256": source_sha256,
        "integration": [
            {
                "operation": step.operation,
                "source": step.identity,
                "destination": step.destination,
                "sha256": sha256_file(step.source),
            }
            for step in inputs
        ],
        "generated": [
            {"destination": name, "sha256": sha256_bytes(contents)}
            for name, contents in sorted(generated.items())
        ],
    }
    return sha256_bytes(common.canonical_json_bytes(payload))


def selected_recipe_digest(
    recipe: str,
    selected: Sequence[LinuxInput],
    inputs: Sequence[LinuxInput],
) -> str:
    """Use the same declared shared-input contract as the early workspace receipt."""
    shared = []
    for step in inputs:
        if step in selected:
            continue
        entry = {
            "operation": step.operation,
            "source": step.identity,
            "destination": step.destination,
        }
        if is_shared_linux_operation(step.operation):
            entry["sha256"] = sha256_file(step.source)
        shared.append(entry)
    return sha256_bytes(common.canonical_json_bytes({"selected": recipe, "shared": shared}))


def publish_projection(
    base: linux_state.LinuxBase,
    inputs: Sequence[LinuxInput],
    generated: dict[str, bytes],
    state: PreparedLinuxState,
) -> None:
    """Compute a complete small projection before updating its changed source files."""
    destinations = {name for step in inputs for name in step.destinations()} | generated.keys()
    # Retained originals restore paths that a removed integration no longer owns.
    names = destinations | linux_state.original_paths(base).keys() | {".clang-format"}
    originals = linux_state.read_originals(base, names)
    with tempfile.TemporaryDirectory(prefix="fplinux-linux-projection-") as temporary:
        projection = Path(temporary)
        for _step, _before, _after in project_changes(originals, inputs, projection):
            pass
        sources_build.write_generated_files(projection, generated, owner="shared Linux")
        projected = file_contents(projection, sorted(names))
        linux_state.invalidate_prepared_linux(base.source)
        for name in sorted(names):
            source = projected.get(name)
            if source is None:
                (base.source / name).unlink(missing_ok=True)
            else:
                linux_state.write_changed_file(base.source / name, source.contents, source.mode)
        linux_state.seal_prepared_linux(base.source, state)


def prepare_linux(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> tuple[Path, PreparedLinuxState]:
    """Prepare all compatible boards in one upstream source slot before its consumers run."""
    source_lock = sources_build.source_lock_entry(sources, platform["linux"]["source_lock"])
    digest = inputs_build.require_sha256(source_lock.get("sha256"), "Linux source")
    targets = discover_linux_targets(common.ROOT, sources, digest)
    selected = next((item for item in targets if item.name == target), None)
    if selected is None:
        fail(f"selected target has no shared Linux integration: {target}")
    selected_inputs = integration_inputs(target, target_config, platform)
    if selected_inputs != integration_inputs(target, selected.config, selected.platform):
        fail(f"target {target}: profile-specific Linux integration cannot share source")
    inputs, generated = shared_integration(targets)
    try:
        base = linux_state.ensure_linux_base(inputs_build.CACHE, source_lock)
        original_paths = linux_state.original_paths(base)
        names = {name for step in inputs for name in step.destinations()} | generated.keys()
        missing = (names | {".clang-format"}) - original_paths.keys()
        if missing:
            linux_state.read_originals(base, missing)
            original_paths = linux_state.original_paths(base)
        for step in inputs:
            if step.operation == "target-copy" and original_paths[step.destination]:
                fail(
                    f"target Linux copy must add a new board file: {step.destination}; "
                    "use a patch or platform copy to change upstream source"
                )
        recipe = selected_recipe_digest(
            linux_recipe_digest(source_lock, target, target_config, platform),
            tuple(LinuxInput(*step) for step in selected_inputs),
            inputs,
        )
        state = PreparedLinuxState(recipe, shared_recipe_digest(digest, inputs, generated))
        if linux_state.inspect_prepared_linux(base.source, state) is None:
            publish_projection(base, inputs, generated, state)
    except LinuxStateError as error:
        fail(str(error))
    return base.source, state
