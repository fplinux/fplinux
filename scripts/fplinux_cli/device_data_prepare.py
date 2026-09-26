# SPDX-License-Identifier: GPL-2.0-only
"""Prepare all target-declared device data from one physical NAND image."""

from __future__ import annotations

import functools
import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.cli.build import build
from fplinux_cli.cli.bundles import resolve_target_bundle
from fplinux_cli.cli.runtime import run_target_noninteractive
from fplinux_cli.manifests.targets import load_target

from .common import ROOT, canonical_json_bytes, fail, replace_file_atomically, sha256_bytes
from .device_data import (
    DeviceDataPreparation,
    NandGeometry,
    PhysicalNand,
    PreparedGroup,
    family_page_bytes,
)
from .firmware_inputs import (
    FirmwareInput,
    capture_device_data_generation,
    device_data_cache_directory,
)
from .nand_backup import backup_target_nand, read_backup_geometry, require_declared_chip
from .output import RunReporter

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping
    from types import ModuleType

# The device-data group that the target's platform extracts from the phone's
# stock firmware, and the platform-owned module that does so.
BOARD_MAPS_GROUP = "board-maps"
BOARD_MAPS_MODULE = "host/stock_image.py"


def _require_directory(path: Path, name: str) -> Path:
    """Create one private cache directory without accepting a linked component."""
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    except OSError as error:
        fail(f"{name} cannot be created: {path}: {error}")
    if path.is_symlink() or not path.is_dir():
        fail(f"{name} is missing or invalid: {path}")
    try:
        path.chmod(0o700)
    except OSError as error:
        fail(f"{name} cannot be made private: {path}: {error}")
    return path


def _create_staging_generation(cache: Path, target: str) -> Path:
    """Allocate the sole private staging generation for one preparation run."""
    if cache.is_symlink() or not cache.is_dir():
        fail(f"cache directory is missing or invalid: {cache}")
    device_data = _require_directory(cache / "device-data", "device-data cache directory")
    target_root = _require_directory(
        device_data / target,
        f"device-data directory for {target}",
    )
    generations = _require_directory(target_root / "generations", "device-data generations")
    try:
        staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=generations))
        staging.chmod(0o700)
    except OSError as error:
        fail(f"device-data staging generation cannot be created: {error}")
    else:
        return staging


def _read_dump(path: Path) -> bytes:
    """Read one regular NAND backup without changing or replacing it."""
    if path.is_symlink() or not path.is_file():
        fail(f"NAND backup is missing or invalid: {path}")
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"NAND backup cannot be read: {path}: {error}")


def dump_page_bytes(
    target: str,
    nand: Mapping[str, Any] | None,
    receipt: NandGeometry | None,
) -> int:
    """Take the physical page size from the backup receipt or the target's declared chip."""
    declared_id = None if nand is None else nand.get("id")
    declared_raw_page_bytes = None if nand is None else nand.get("raw_page_bytes")
    if receipt is None:
        if declared_raw_page_bytes is None:
            fail(
                f"target {target} declares no NAND chip and the backup has no geometry "
                "receipt; save it with ./fplinux nand backup, which writes PATH.json beside it"
            )
        return int(declared_raw_page_bytes)
    try:
        page_bytes = family_page_bytes(receipt)
    except ValueError as error:
        fail(f"device-data extraction failed: {error}")
    require_declared_chip(
        receipt,
        target=target,
        declared_id=declared_id,
        declared_raw_page_bytes=declared_raw_page_bytes,
        source="NAND backup receipt",
    )
    return page_bytes


def _load_source_module(path: Path, name: str, description: str) -> ModuleType:
    """Load one project-owned Python source through its normal module boundary."""
    if path.is_symlink() or not path.is_file():
        fail(f"{description} is missing or invalid: {path}")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"{description} cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


def _load_device_data_parser(target: str, filename: str) -> ModuleType:
    """Load one target-owned parser through its normal source module boundary."""
    module = _load_source_module(
        ROOT / "targets" / target / filename,
        f"fplinux_{target.replace('-', '_')}_device_data",
        "device-data parser",
    )
    if not callable(getattr(module, "prepare_device_data", None)):
        fail("device-data parser does not expose prepare_device_data(nand)")
    return module


def _load_board_maps_provider(platform: str) -> ModuleType:
    """Load the platform module that extracts board maps from the stock firmware."""
    module = _load_source_module(
        ROOT / "platforms" / platform / BOARD_MAPS_MODULE,
        f"fplinux_{platform.replace('-', '_')}_stock_image",
        f"platform {platform} {BOARD_MAPS_GROUP} module",
    )
    if not callable(getattr(module, "prepare_board_maps", None)):
        fail(
            f"platform {platform} {BOARD_MAPS_GROUP} module does not expose "
            "prepare_board_maps(nand, host_tools=...)"
        )
    return module


def _board_maps_extractor(target: str, platform: str) -> Callable[[PhysicalNand], object]:
    """Bind the platform extraction to the host tools of the target's current build."""
    provider = _load_board_maps_provider(platform)
    bundle, _manifest = resolve_target_bundle(target)
    extract: Callable[[PhysicalNand], object] = functools.partial(
        provider.prepare_board_maps, host_tools=bundle.path / "host"
    )
    return extract


def _canonical_files(
    value: object,
    *,
    group: str,
    label: str,
    expected_names: tuple[str, ...] | None,
) -> dict[str, bytes]:
    """Require one path-safe mapping of immutable parser output bytes."""
    if not isinstance(value, dict) or not value:
        fail(f"device-data group {group} returned an invalid {label} set")
    if expected_names is not None and set(value) != set(expected_names):
        expected = ", ".join(expected_names)
        fail(f"device-data group {group} returned an invalid {label} set; expected: {expected}")
    result: dict[str, bytes] = {}
    for name, contents in value.items():
        if not isinstance(name, str) or not name or Path(name).name != name:
            fail(f"device-data group {group} returned an unsafe {label} filename")
        if not isinstance(contents, bytes):
            fail(f"device-data group {group} returned non-byte {label} contents")
        result[name] = contents
    return result


def _extract_groups(
    target: str,
    parser_filename: str | None,
    groups: Mapping[str, list[dict[str, Any]]],
    nand: PhysicalNand,
    *,
    board_maps: Callable[[PhysicalNand], object] | None,
) -> dict[str, PreparedGroup]:
    """Run the target parser and the platform board-map extraction once each.

    Every requested group is then normalized.
    """
    produced: dict[str, object] = {}
    parser_groups = set(groups) - {BOARD_MAPS_GROUP}
    if parser_groups:
        parser = _load_device_data_parser(target, str(parser_filename))
        try:
            result = parser.prepare_device_data(nand)
        except ValueError as error:
            fail(f"device-data extraction failed: {error}")
        if not isinstance(result, DeviceDataPreparation) or set(result.groups) != parser_groups:
            expected = ", ".join(sorted(parser_groups))
            fail(f"device-data parser returned an invalid group set; expected: {expected}")
        produced.update(result.groups)
    if board_maps is not None:
        try:
            produced[BOARD_MAPS_GROUP] = board_maps(nand)
        except ValueError as error:
            fail(f"device-data extraction failed: {error}")

    normalized: dict[str, PreparedGroup] = {}
    for group_name in sorted(groups):
        prepared_group = produced[group_name]
        if not isinstance(prepared_group, PreparedGroup):
            fail(f"device-data group {group_name} returned an invalid result")
        expected_names = tuple(str(declaration["source"]) for declaration in groups[group_name])
        originals = _canonical_files(
            prepared_group.originals,
            group=group_name,
            label="original",
            expected_names=None,
        )
        prepared = _canonical_files(
            prepared_group.prepared,
            group=group_name,
            label="prepared",
            expected_names=expected_names,
        )
        reports = (
            _canonical_files(
                prepared_group.reports,
                group=group_name,
                label="report",
                expected_names=None,
            )
            if prepared_group.reports
            else {}
        )
        normalized[group_name] = PreparedGroup(
            originals=originals, prepared=prepared, reports=reports
        )
    return normalized


def _publish_files(directory: Path, files: Mapping[str, bytes], name: str) -> None:
    """Write one complete parser output set into a new private directory."""
    destination = _require_directory(directory, name)
    for filename, contents in files.items():
        replace_file_atomically(destination / filename, contents, 0o600)


def _materialize_groups(staging: Path, groups: Mapping[str, PreparedGroup]) -> None:
    """Save each group's originals, prepared bytes and reports in a new generation."""
    originals = _require_directory(staging / "originals", "device-data originals directory")
    prepared = _require_directory(staging / "groups", "device-data group directory")
    for group_name, group in groups.items():
        _publish_files(
            originals / group_name,
            group.originals,
            f"device-data originals for {group_name}",
        )
        _publish_files(
            prepared / group_name,
            group.prepared,
            f"device-data prepared files for {group_name}",
        )
    reported = {name: group.reports for name, group in groups.items() if group.reports}
    if reported:
        reports = _require_directory(staging / "reports", "device-data report directory")
        for group_name, files in reported.items():
            _publish_files(reports / group_name, files, f"device-data reports for {group_name}")


def _write_receipt(
    staging: Path,
    *,
    source_kind: str,
    raw: bytes,
    groups: Mapping[str, PreparedGroup],
    admitted: Mapping[str, tuple[FirmwareInput, ...]],
) -> None:
    """Record hashes and sizes without exposing source paths or private contents."""
    group_records: list[dict[str, Any]] = []
    for group_name in sorted(groups):
        group = groups[group_name]
        group_record: dict[str, Any] = {
            "name": group_name,
            "originals": [
                {
                    "source": name,
                    "size": len(contents),
                    "sha256": sha256_bytes(contents),
                }
                for name, contents in group.originals.items()
            ],
            "prepared": [firmware.recipe_record() for firmware in admitted[group_name]],
        }
        if group.reports:
            group_record["reports"] = [
                {"name": name, "size": len(contents), "sha256": sha256_bytes(contents)}
                for name, contents in group.reports.items()
            ]
        group_records.append(group_record)
    record = {
        "source": {
            "kind": source_kind,
            "size": len(raw),
            "sha256": sha256_bytes(raw),
        },
        "groups": group_records,
    }
    replace_file_atomically(staging / "receipt.json", canonical_json_bytes(record), 0o600)


def _select_generation(cache: Path, target: str, staging: Path) -> Path:
    """Seal the staged directory before atomically switching the small current pointer."""
    suffix = staging.name.removeprefix(".staging-")
    generation = staging.with_name(f"generation-{suffix}")
    target_root = device_data_cache_directory(cache, target)
    try:
        staging.replace(generation)
        replace_file_atomically(target_root / "current", f"{generation.name}\n".encode(), 0o600)
    except OSError as error:
        if generation.exists() and not staging.exists():
            shutil.rmtree(generation)
        fail(f"device-data generation cannot be selected: {error}")
    return generation


def prepare_device_data(
    target: str,
    *,
    from_dump: Path | None,
    jobs: int,
    offline: bool,
) -> None:
    """Prepare every declared group from one live or previously saved physical NAND."""
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
    # Board maps are extracted with a host tool from the target's current build.
    board_maps = (
        _board_maps_extractor(target, str(target_config["platform"]))
        if BOARD_MAPS_GROUP in declarations
        else None
    )
    if from_dump is None:
        print(
            "Connect the powered-off phone only when the loader asks.",
            file=sys.stderr,
            flush=True,
        )
        with reporter.stage("load", show_tail=False):
            run_target_noninteractive(target, profile=None)
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
