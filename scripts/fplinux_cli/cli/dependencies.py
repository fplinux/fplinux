# SPDX-License-Identifier: GPL-2.0-only
"""Create, verify and restore checkout-selected dependency snapshots."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.dependencies.environment_archive import restore_environment, save_environment
from fplinux_cli.dependencies.inputs import DependencyInput, dependency_selection
from fplinux_cli.dependencies.site_inputs import (
    remember_site_inputs,
    resolve_site_inputs,
    site_dependency_context,
)
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


def _checkout_declarations(
    *,
    manifest: dict[str, Any] | None = None,
    offline: bool,
    sources: Sequence[Path] = (),
) -> tuple[list[DependencyInput], dict[str, Any]]:
    inputs, context = dependency_selection(common.ROOT)
    context["site"] = site_dependency_context(common.ROOT)
    saved_site = None
    if manifest is not None:
        saved_site = [
            DependencyInput(**record["input"])
            for record in manifest["inputs"]
            if record["input"]["purpose"] == "site-python"
        ]
    inputs.extend(
        resolve_site_inputs(
            common.ROOT, offline=offline, saved_inputs=saved_site, source_directories=sources
        )
    )
    return sorted(inputs, key=lambda item: item.key), context


def create_dependencies(
    directory: Path, *, offline: bool, sources: Sequence[Path], inputs_only: bool
) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("dependency-selection"):
        inputs, context = _checkout_declarations(offline=offline, sources=sources)
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
    current_inputs, current_context = _checkout_declarations(manifest=manifest, offline=True)
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
        inputs, context = _checkout_declarations(manifest=manifest, offline=True)
        require_checkout(manifest, inputs, context)
    reporter.finish()
    print(f"Verified dependency snapshot: {manifest['snapshot']}")


def restore_dependencies(directory: Path, *, inputs_only: bool) -> None:
    directory = directory.absolute()
    reporter = RunReporter.create("dependencies", target=None, verbose=False)
    with reporter.stage("restore-inputs"):
        manifest = read_snapshot(directory)
        inputs, context = _checkout_declarations(manifest=manifest, offline=True)
        require_checkout(manifest, inputs, context)
        restore_manifest_inputs(directory, manifest, common.ROOT / ".cache")
        remember_site_inputs(
            common.ROOT, [item for item in inputs if item.purpose == "site-python"]
        )
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
