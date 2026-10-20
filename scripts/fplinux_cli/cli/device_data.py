# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate explicit developer acquisition, preparation and board reports."""

from __future__ import annotations

import contextlib
import shutil
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.workflow import build, prepare_host_tool
from fplinux_cli.common import ROOT, fail
from fplinux_cli.device_data.board_report import BOARD_REPORT_MODULE, write_board_report
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.prepare import (
    NAND_ECC_MODULE,
    NAND_ECC_TOOL,
    DeviceDataSpec,
    DeviceDataTools,
    _require_directory,
    prepare_from_nand,
    prepare_from_originals,
)
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.runtime.runner import prepare_target
from fplinux_cli.runtime.workflow import launcher_lock

if TYPE_CHECKING:
    from collections.abc import Iterator, Mapping


def _target_spec(target: str, config: Mapping[str, Any]) -> DeviceDataSpec:
    """Resolve checkout-owned module paths before entering the shared extractor."""
    declaration = config["device_data"]
    parser = declaration.get("parser")
    originals_parser = declaration.get("originals_parser")
    nand = config.get("nand")
    return DeviceDataSpec(
        target=target,
        parser=None if parser is None else ROOT / "targets" / target / str(parser),
        originals_parser=(
            None if originals_parser is None else ROOT / "targets" / target / str(originals_parser)
        ),
        groups=declaration["groups"],
        audio=AudioProfileContext(
            machine_compatible=config["identity"]["compatible"].encode("ascii"),
            **config["audio_policy"],
        ),
        nand=nand,
        ecc_module=(
            ROOT / "platforms" / str(config["platform"]) / NAND_ECC_MODULE
            if nand is not None and "ecc_format" in nand
            else None
        ),
    )


def _data_directory(target: str) -> Path:
    """Create the checkout's flat private data root without touching existing backups."""
    cache = _require_directory(ROOT / ".cache", "cache directory")
    data = _require_directory(cache / "device-data", "device-data cache directory")
    return _require_directory(data / target, f"device-data directory for {target}")


@contextlib.contextmanager
def _saved_source_tools(
    spec: DeviceDataSpec,
    platform: str,
    *,
    offline: bool,
    reporter: RunReporter,
    board_report: bool = False,
) -> Iterator[DeviceDataTools]:
    """Prepare only the native tools needed by this explicit saved-source command."""
    required = [NAND_ECC_TOOL] if spec.ecc_module is not None else []
    if board_report:
        required.append("fphelper_t117")
    if not required:
        yield DeviceDataTools()
        return
    with tempfile.TemporaryDirectory(prefix="device-data-host-", dir=ROOT / ".cache") as temporary:
        host_tools = Path(temporary)
        for tool in required:
            prepare_host_tool(
                spec.target, platform, tool, host_tools, offline=offline, reporter=reporter
            )
        yield DeviceDataTools(host_tools=host_tools)


def prepare_device_data(  # noqa: PLR0913 -- explicit source, build and loader controls.
    target: str,
    *,
    from_dump: Path | None,
    jobs: int,
    offline: bool,
    from_originals: Path | None = None,
    events: Path | None = None,
) -> None:
    """Use the shared extractor and replace phone data under the command's locks."""
    if from_dump is not None and from_originals is not None:
        fail("--from-dump and --from-originals are mutually exclusive")
    if events is not None and (from_dump is not None or from_originals is not None):
        fail("--events requires a live device-data load and cannot be used with a saved source")
    config = load_target(target)
    declaration = config["device_data"]
    if not declaration["groups"]:
        fail(f"device-data preparation is not supported for target {target}")
    if declaration.get("originals_parser") is not None:
        if from_originals is None:
            fail(f"target {target} requires --from-originals MANIFEST.json")
    elif from_originals is not None:
        fail(f"target {target} requires physical NAND data and does not support --from-originals")
    spec = _target_spec(target, config)
    reporter = RunReporter.create("device-data", target=target, verbose=False)
    live_nand = from_dump is None and from_originals is None
    if live_nand:
        with reporter.stage("build", show_tail=False):
            build(target, jobs, offline=offline, reporter=reporter)
        with reporter.stage("prepare", show_tail=False):
            prepare_target(target, profile=None, replace=True, events=events)
        reporter.finish()
        _show_prepared_data(
            target,
            ROOT / ".cache" / "device-data" / target / "phone",
            requires_build=False,
        )
        return

    data = _data_directory(target)
    destination = data / "phone"
    with launcher_lock(data):
        if from_originals is not None:
            with reporter.stage("prepare"):
                prepare_from_originals(
                    spec, from_originals, destination, DeviceDataTools(), replace=True
                )
        elif from_dump is not None:
            with (
                _saved_source_tools(
                    spec, str(config["platform"]), offline=offline, reporter=reporter
                ) as tools,
                reporter.stage("prepare"),
            ):
                prepare_from_nand(spec, from_dump, destination, tools, replace=True)
    reporter.finish()
    _show_prepared_data(target, destination, requires_build=from_originals is not None)


def _show_prepared_data(target: str, destination: Path, *, requires_build: bool) -> None:
    """Show the stable preparation result and the next checkout commands."""
    print(f"Device data is ready in {destination}.")
    reports = destination / "reports"
    if reports.is_dir():
        for report in sorted(reports.glob("*/*")):
            print(f"Review {report}.")
    print("Next:")
    if requires_build:
        print(f"  ./fplinux build {target}")
    print(f"  ./fplinux run {target}")
    if not requires_build:
        print(
            "For a microSD system card, rebuild that card with its selected profile "
            "before booting it."
        )


def report_device_data(target: str, *, from_dump: Path, offline: bool) -> None:
    """Write a contributor board report without requiring runtime data groups."""
    config = load_target(target)
    platform = str(config["platform"])
    extractor = ROOT / "platforms" / platform / BOARD_REPORT_MODULE
    if extractor.is_symlink() or not extractor.is_file():
        fail(f"board reports are not supported for platform {platform}")
    spec = _target_spec(target, config)
    reporter = RunReporter.create("device-data", target=target, verbose=False)
    cache = _require_directory(ROOT / ".cache", "cache directory")
    diagnostics = _require_directory(cache / "diagnostics", "diagnostics directory")
    reports = _require_directory(diagnostics / "device-data", "device-data reports directory")
    target_reports = _require_directory(reports / target, f"board reports for {target}")
    destination = Path(tempfile.mkdtemp(prefix="report-", dir=target_reports))
    try:
        with (
            _saved_source_tools(
                spec, platform, offline=offline, reporter=reporter, board_report=True
            ) as tools,
            reporter.stage("report"),
        ):
            report = write_board_report(spec, from_dump, destination, tools, extractor=extractor)
    except BaseException:
        shutil.rmtree(destination)
        raise
    reporter.finish()
    print(f"Review {report}.")
