# SPDX-License-Identifier: GPL-2.0-only
"""Acquire NAND data and coordinate target-owned preparation and publication."""

from __future__ import annotations

import contextlib
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

from fplinux_cli.cli.build import build, prepare_host_tool
from fplinux_cli.common import ROOT, fail, replace_file_atomically
from fplinux_cli.device_data.formats import PhysicalNand
from fplinux_cli.device_data.inputs import capture_device_data_generation
from fplinux_cli.device_data.prepare import (
    BOARD_MAPS_GROUP,
    _board_maps_extractor,
    _create_staging_generation,
    _extract_groups,
    _materialize_groups,
    _read_dump,
    _require_directory,
    _select_generation,
    _write_receipt,
    dump_page_bytes,
)
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.runtime.bundle_session import resolve_target_bundle
from fplinux_cli.runtime.nand_backup import backup_target_nand, read_backup_geometry
from fplinux_cli.runtime.runner import run_target_noninteractive


def prepare_device_data(
    target: str,
    *,
    from_dump: Path | None,
    jobs: int,
    offline: bool,
    events: Path | None = None,
) -> None:
    """Prepare every declared group from one live or previously saved physical NAND."""
    if from_dump is not None and events is not None:
        fail("--events requires a live device-data load and cannot be used with --from-dump")
    target_config = load_target(target)
    device_data = target_config["device_data"]
    declarations: dict[str, list[dict[str, Any]]] = device_data["groups"]
    if not declarations:
        fail(f"device-data preparation is not supported for target {target}")
    parser_filename: str | None = device_data.get("parser")
    nand_declaration: dict[str, Any] | None = target_config.get("nand")
    cache = ROOT / ".cache"
    reporter = RunReporter.create("device-data", target=target, verbose=False)

    if from_dump is None:
        with reporter.stage("build", show_tail=False):
            build(target, jobs, offline=offline, reporter=reporter)
    # A live acquisition uses the host tool already built with its loader.
    board_maps = None
    if from_dump is None and BOARD_MAPS_GROUP in declarations:
        bundle, _manifest = resolve_target_bundle(target)
        board_maps = _board_maps_extractor(
            str(target_config["platform"]), host_tools=bundle.path / "host"
        )
    if from_dump is None:
        print(
            "Connect the powered-off phone only when the loader asks.",
            file=sys.stderr,
            flush=True,
        )
        with reporter.stage("load", show_tail=False):
            run_target_noninteractive(target, profile=None, events=events)
        source_kind = "live-nand"
    else:
        source_kind = "saved-dump"

    staging = _create_staging_generation(cache, target)
    selected = False
    try:
        with reporter.stage("prepare"):
            source_directory = _require_directory(
                staging / "source",
                "device-data source directory",
            )
        source = source_directory / "nand.bin"
        if from_dump is None:
            backup_target_nand(target, source, reporter=reporter)
            dump = source
            with reporter.stage("read-dump"):
                raw = _read_dump(source)
        else:
            dump = from_dump
            with reporter.stage("read-dump"):
                raw = _read_dump(from_dump)
                replace_file_atomically(source, raw, 0o600)

        with reporter.stage("extract"):
            page_bytes = dump_page_bytes(
                target,
                nand_declaration,
                read_backup_geometry(dump, raw),
            )
            try:
                nand = PhysicalNand.from_dump(raw, page_bytes=page_bytes)
            except ValueError as error:
                fail(f"device-data extraction failed: {error}")
            with contextlib.ExitStack() as resources:
                if from_dump is not None and BOARD_MAPS_GROUP in declarations:
                    host_tools = Path(
                        resources.enter_context(
                            tempfile.TemporaryDirectory(prefix="device-data-host-", dir=cache)
                        )
                    )
                    platform = str(target_config["platform"])
                    prepare_host_tool(
                        target,
                        platform,
                        "fphelper_t117",
                        host_tools,
                        offline=offline,
                        reporter=reporter,
                    )
                    board_maps = _board_maps_extractor(platform, host_tools=host_tools)
                extracted = _extract_groups(
                    target,
                    parser_filename,
                    declarations,
                    nand,
                    board_maps=board_maps,
                )
            _materialize_groups(staging, extracted)
            admitted = capture_device_data_generation(
                declarations,
                staging,
                path_field="source",
                require_all=True,
            )
        with reporter.stage("publish"):
            _write_receipt(
                staging,
                source_kind=source_kind,
                raw=raw,
                groups=extracted,
                admitted=admitted,
            )
            generation = _select_generation(cache, target, staging)
            selected = True
    finally:
        if not selected and staging.exists():
            shutil.rmtree(staging)

    reporter.finish()
    print(f"Device data is ready in {generation}.")
    for group_name in sorted(extracted):
        for report_name in extracted[group_name].reports:
            print(f"Review {generation / 'reports' / group_name / report_name}.")
    print("Next:")
    print(f"  ./fplinux build {target}")
    print(f"  ./fplinux run {target}")
