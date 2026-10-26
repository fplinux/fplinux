# SPDX-License-Identifier: GPL-2.0-only
"""Coordinate explicit developer acquisition, preparation and board reports."""

from __future__ import annotations

import contextlib
import shlex
import shutil
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.phone_data import preparation_recipe
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
    import_phone_backup,
    prepare_from_nand,
    prepare_from_originals,
)
from fplinux_cli.device_data.store import locked_phone_store, phone_store_directory
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.runtime.runner import prepare_target

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
        preparation_recipe=(
            preparation_recipe(target, dict(config), source_root=ROOT)
            if declaration["groups"]
            else None
        ),
        nand=nand,
        emmc=config.get("emmc"),
        required_groups=tuple(declaration.get("required_groups", declaration["groups"])),
        ecc_module=(
            ROOT / "platforms" / str(config["platform"]) / NAND_ECC_MODULE
            if nand is not None and "ecc_format" in nand
            else None
        ),
    )


@contextlib.contextmanager
def _saved_source_tools(
    spec: DeviceDataSpec,
    platform: str,
    *,
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
            prepare_host_tool(spec.target, platform, tool, host_tools, reporter=reporter)
        yield DeviceDataTools(host_tools=host_tools)


def prepare_device_data(  # noqa: PLR0913 -- explicit source, build and loader controls.
    target: str,
    *,
    from_dump: Path | None,
    jobs: int,
    from_originals: Path | None = None,
    events: Path | None = None,
    phone_data: Path | None = None,
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
    if declaration.get("originals_parser") is not None and "emmc" not in config:
        if from_originals is None:
            fail(f"target {target} requires --from-originals MANIFEST.json")
    elif declaration.get("originals_parser") is None and from_originals is not None:
        fail(f"target {target} requires physical NAND data and does not support --from-originals")
    spec = _target_spec(target, config)
    reporter = RunReporter.create("device-data", target=target, verbose=False)
    runtime_preparation = from_originals is None and (from_dump is None or "emmc" in config)
    if runtime_preparation:
        with reporter.stage("build", show_tail=False):
            build(target, jobs, reporter=reporter, phone_data=phone_data)
        with reporter.stage("prepare", show_tail=False):
            prepare_target(
                target,
                profile=None,
                replace=True,
                events=events,
                phone_data=phone_data,
                from_dump=from_dump,
            )
        reporter.finish()
        _show_prepared_data(
            target,
            phone_store_directory(target, root=phone_data) / "phone",
            phone_data=phone_data,
        )
        return

    destination = phone_store_directory(target, root=phone_data) / "phone"
    if from_originals is not None:
        with locked_phone_store(target, root=phone_data), reporter.stage("prepare"):
            prepare_from_originals(
                spec, from_originals, destination, DeviceDataTools(), replace=True
            )
    elif from_dump is not None:
        with (
            _saved_source_tools(spec, str(config["platform"]), reporter=reporter) as tools,
            locked_phone_store(target, root=phone_data) as store,
            reporter.stage("prepare"),
        ):
            backup = import_phone_backup(
                spec, tools, from_dump, store / "nand.bin", acquisition_recipe=None
            )
            prepare_from_nand(spec, backup, destination, tools, replace=True)
    reporter.finish()
    _show_prepared_data(target, destination, phone_data=phone_data)


def _show_prepared_data(target: str, destination: Path, *, phone_data: Path | None = None) -> None:
    """Show the stable preparation result and the next checkout commands."""
    print(f"Device data is ready in {destination}.")
    reports = destination / "reports"
    if reports.is_dir():
        for report in sorted(reports.glob("*/*")):
            print(f"Review {report}.")
    print("Next:")
    selector = "" if phone_data is None else f" --phone-data {shlex.quote(str(phone_data))}"
    print(f"  ./fplinux run {target}{selector}")
    print(
        "For a microSD system card, rebuild that card with its selected profile before booting it."
    )


def report_device_data(target: str, *, from_dump: Path) -> None:
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
            _saved_source_tools(spec, platform, reporter=reporter, board_report=True) as tools,
            reporter.stage("report"),
        ):
            report = write_board_report(spec, from_dump, destination, tools, extractor=extractor)
    except BaseException:
        shutil.rmtree(destination)
        raise
    reporter.finish()
    print(f"Review {report}.")
