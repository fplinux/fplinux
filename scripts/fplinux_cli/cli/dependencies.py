# SPDX-License-Identifier: GPL-2.0-only
"""Create, verify and restore checkout-selected dependency snapshots."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
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
from fplinux_cli.reporting.run import RunReporter

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def _checkout_declarations() -> tuple[list[DependencyInput], dict[str, Any]]:
    inputs, context = dependency_selection(common.ROOT)
    site_inputs, context["site"] = site_dependency_selection(common.ROOT)
    inputs.extend(site_inputs)
    return sorted(inputs, key=lambda item: item.key), context


def create_dependencies(
    directory: Path, *, offline: bool, sources: Sequence[Path], inputs_only: bool
) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("dependency-selection"):
        inputs, context = _checkout_declarations()
    with reporter.stage("preserve-inputs"):
        manifest = preserve_inputs(
            inputs,
            context,
            directory,
            cache=common.ROOT / ".cache",
            offline=offline,
            sources=sources,
        )
    if not inputs_only:
        manifest["environment"] = save_environment(directory, reporter)
    current_inputs, current_context = _checkout_declarations()
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
        inputs, context = _checkout_declarations()
        require_checkout(manifest, inputs, context)
    reporter.finish()
    print(f"Verified dependency snapshot: {manifest['snapshot']}")


def restore_dependencies(directory: Path, *, inputs_only: bool) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("restore-inputs"):
        manifest = read_snapshot(directory)
        inputs, context = _checkout_declarations()
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
