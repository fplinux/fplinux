# SPDX-License-Identifier: GPL-2.0-only
"""Create, verify and restore checkout-selected dependency snapshots."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.release import capture_public_sources
from fplinux_cli.common import fail
from fplinux_cli.dependencies.distribution import (
    read_dependency_distribution,
    write_dependency_distribution,
)
from fplinux_cli.dependencies.environment_archive import restore_environment, save_environment
from fplinux_cli.dependencies.inputs import DependencyInput, dependency_selection
from fplinux_cli.dependencies.site_inputs import site_dependency_selection
from fplinux_cli.dependencies.snapshots import (
    preserve_inputs,
    publish_snapshot,
    read_snapshot,
    require_checkout,
    restore_manifest_inputs,
)
from fplinux_cli.dependencies.transport import export_snapshot, restore_transport
from fplinux_cli.reporting.run import RunReporter

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def checkout_declarations() -> tuple[list[DependencyInput], dict[str, Any]]:
    """Resolve all current build and documentation consumers through their owners."""
    inputs, context = dependency_selection(common.ROOT)
    site_inputs, context["site"] = site_dependency_selection(common.ROOT)
    selected = {item.key: item for item in inputs}
    for item in site_inputs:
        previous = selected.get(item.key)
        if previous is not None and previous != item:
            fail(f"dependency has conflicting build and documentation consumers: {item.key}")
        selected[item.key] = item
    return [selected[key] for key in sorted(selected)], context


def create_dependencies(
    directory: Path, *, offline: bool, sources: Sequence[Path], inputs_only: bool
) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("dependency-selection"):
        inputs, context = checkout_declarations()
    with reporter.stage("preserve-inputs") as stage:

        def report_download(input_file: DependencyInput, position: int, total: int) -> None:
            message = (
                f"dependencies: download {position}/{total}: {input_file.key}: {input_file.url}"
            )
            stage.write((message + "\n").encode())
            print(message, file=sys.stderr, flush=True)

        manifest = preserve_inputs(
            inputs,
            context,
            directory,
            cache=common.ROOT / ".cache",
            offline=offline,
            sources=sources,
            on_download=report_download,
        )
    if not inputs_only:
        manifest["environment"] = save_environment(directory, reporter)
    current_inputs, current_context = checkout_declarations()
    if current_inputs != inputs or current_context != context:
        fail("external dependency declarations changed while the snapshot was being created")
    with reporter.stage("snapshot-verify"):
        publish_snapshot(directory, manifest)
        read_snapshot(directory)
    reporter.finish()
    print(f"Dependency snapshot: {manifest['snapshot']}")


def verify_dependencies(directory: Path) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("snapshot-verify"):
        manifest = read_snapshot(directory)
        inputs, context = checkout_declarations()
        require_checkout(manifest, inputs, context)
    reporter.finish()
    print(f"Verified dependency snapshot: {manifest['snapshot']}")


def restore_dependencies(directory: Path, *, inputs_only: bool) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("restore-inputs"):
        manifest = read_snapshot(directory)
        inputs, context = checkout_declarations()
        require_checkout(manifest, inputs, context)
        restore_manifest_inputs(directory, manifest, common.ROOT / ".cache")
    if not inputs_only:
        environment = manifest["environment"]
        if environment is None:
            fail(
                "snapshot has no saved build environment; "
                "use --inputs-only to restore its external inputs"
            )
        restore_environment(directory, environment, reporter)
    reporter.finish()
    print(f"Restored dependency snapshot: {manifest['snapshot']}")


def export_dependencies(directory: Path, output: Path) -> None:
    """Publish verified inputs-only snapshot transport parts."""
    directory, output = directory.absolute(), output.absolute()
    manifest = read_snapshot(directory)
    inputs, context = checkout_declarations()
    require_checkout(manifest, inputs, context)
    release = capture_public_sources().release
    index = export_snapshot(directory, output, prefix=f"FPLinux-{release.release_id}-inputs")
    write_dependency_distribution(
        output, release=release, snapshot=manifest["snapshot"], index=index
    )
    print(f"Dependency transport: {index}")


def import_dependencies(directory: Path, output: Path) -> None:
    """Authenticate the release association before restoring its original snapshot."""
    directory = directory.absolute()
    distribution = read_dependency_distribution(directory, verify_parts=False)
    release = capture_public_sources().release
    if distribution.release != release:
        fail("dependency transport belongs to another source release")
    snapshot = restore_transport(
        directory,
        output.absolute(),
        index_name=distribution.index,
        index_sha256=distribution.index_sha256,
    )
    manifest = read_snapshot(snapshot)
    if manifest["snapshot"] != distribution.snapshot:
        fail("dependency transport snapshot differs from its release metadata")
    inputs, context = checkout_declarations()
    require_checkout(manifest, inputs, context)
    print(f"Dependency snapshot: {snapshot}")
