# SPDX-License-Identifier: GPL-2.0-only
"""Extract, admit and publish the same phone data in a checkout or delivered runtime."""

from __future__ import annotations

import functools
import importlib.util
import os
import shutil
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from types import MappingProxyType
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_bytes
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
)
from fplinux_cli.device_data.inputs import capture_phone_data
from fplinux_cli.device_data.nand_geometry import family_page_bytes
from fplinux_cli.device_data.original_files import read_original_files
from fplinux_cli.runtime.nand_backup import (
    backup_receipt_path,
    read_backup_geometry,
    read_backup_record,
    require_declared_chip,
    save_backup_bytes,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping
    from types import ModuleType

    from fplinux_cli.device_data.formats import AudioProfileContext
    from fplinux_cli.device_data.inputs import FirmwareInput
    from fplinux_cli.device_data.nand_geometry import NandGeometry

NAND_ECC_TOOL = "ums9117-nand-ecc"
NAND_ECC_MODULE = "host/nand_ecc.py"


@dataclass(frozen=True)
class DeviceDataSpec:
    """Selected target policy and the exact parser modules supplied by its caller."""

    target: str
    parser: Path | None
    groups: Mapping[str, list[dict[str, Any]]]
    audio: AudioProfileContext
    preparation_recipe: str | None
    nand: Mapping[str, Any] | None = None
    originals_parser: Path | None = None
    ecc_module: Path | None = None
    emmc: Mapping[str, Any] | None = None
    required_groups: tuple[str, ...] | None = None

    @property
    def required_declarations(self) -> Mapping[str, list[dict[str, Any]]]:
        """Select data needed for normal boot, leaving separately imported groups optional."""
        if self.required_groups is None:
            return self.groups
        return {name: self.groups[name] for name in self.required_groups}


@dataclass(frozen=True)
class DeviceDataTools:
    """Prepared native tools and the host's private-directory policy."""

    host_tools: Path | None = None
    protect_private: Callable[[Path], None] | None = None
    python_modules: Path | None = None


def _require_directory(path: Path, name: str) -> Path:
    """Create one private directory without accepting a linked component."""
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    except OSError as error:
        fail(f"{name} cannot be created: {path}: {error}")
    if path.is_symlink() or not path.is_dir():
        fail(f"{name} is missing or invalid: {path}")
    if os.name != "nt":
        try:
            path.chmod(0o700)
        except OSError as error:
            fail(f"{name} cannot be made private: {path}: {error}")
    return path


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
    """Take physical page size from the backup receipt or the target's declared chip."""
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
    """Load one selected Python source through its normal module boundary."""
    if path.is_symlink() or not path.is_file():
        fail(f"{description} is missing or invalid: {path}")
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        fail(f"{description} cannot be loaded: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    try:
        specification.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


def _load_device_data_parser(target: str, path: Path) -> ModuleType:
    """Load the selected target parser without inferring a checkout location."""
    module = _load_source_module(
        path, f"fplinux_{target.replace('-', '_')}_device_data", "device-data parser"
    )
    if not callable(getattr(module, "prepare_device_data", None)):
        fail("device-data parser does not expose prepare_device_data(nand, audio=...)")
    return module


def _load_originals_parser(target: str, path: Path) -> ModuleType:
    """Load the selected named-original preparer without a NAND interface."""
    module = _load_source_module(
        path, f"fplinux_{target.replace('-', '_')}_originals", "device-data originals parser"
    )
    if not callable(getattr(module, "prepare_from_originals", None)):
        fail("device-data originals parser does not expose prepare_from_originals(originals)")
    return module


def _nand_main_decoder(
    path: Path,
    *,
    host_tools: Path,
) -> Callable[[bytes, int, int, int], bytes]:
    """Bind the selected platform ECC module to the prepared native tool."""
    provider = _load_source_module(path, "fplinux_selected_nand_ecc", "NAND ECC module")
    if not callable(getattr(provider, "decode_main", None)):
        fail("NAND ECC module does not expose decode_main")
    decode: Callable[[bytes, int, int, int], bytes] = functools.partial(
        provider.decode_main, host_tools=host_tools
    )
    return decode


def read_physical_nand(spec: DeviceDataSpec, backup: Path, tools: DeviceDataTools) -> PhysicalNand:
    """Admit a complete backup and its geometry before any parser can consume it."""
    raw = _read_dump(backup)
    page_bytes = dump_page_bytes(spec.target, spec.nand, read_backup_geometry(backup, raw))
    main_decoder = None
    if spec.nand is not None and "ecc_format" in spec.nand:
        if spec.ecc_module is None or tools.host_tools is None:
            fail("device-data extraction requires the declared NAND ECC module and host tool")
        main_decoder = _nand_main_decoder(spec.ecc_module, host_tools=tools.host_tools)
    try:
        return PhysicalNand.from_dump(raw, page_bytes=page_bytes, main_decoder=main_decoder)
    except ValueError as error:
        fail(f"device-data extraction failed: {error}")


def import_phone_backup(
    spec: DeviceDataSpec,
    tools: DeviceDataTools,
    source: Path,
    destination: Path,
    *,
    acquisition_recipe: str | None,
) -> Path:
    """Retain an explicitly admitted raw source without inventing its acquisition identity."""
    from fplinux_cli.device_data.nand_geometry import (  # noqa: PLC0415 -- physical import only.
        PAGE_MAIN_BYTES,
        PAGES_PER_BLOCK,
        PHYSICAL_PAGE_COUNT,
        NandGeometry,
    )

    physical = read_physical_nand(spec, source, tools)
    record = read_backup_record(source, physical.raw)
    if record is not None:
        if record.target != spec.target:
            fail("The imported backup belongs to another phone target")
        geometry = record.geometry
        selected_recipe = acquisition_recipe or record.acquisition_recipe
    elif acquisition_recipe is None:
        replace_file_atomically(destination, physical.raw, 0o600)
        backup_receipt_path(destination).unlink(missing_ok=True)
        return destination
    else:
        declared_id = None if spec.nand is None else spec.nand.get("id")
        if declared_id is None:
            fail("Importing this phone's backup requires its NAND geometry receipt")
        geometry = NandGeometry(
            id_bytes=int(declared_id).to_bytes(2, "little").hex(),
            chip="target-declared",
            page_main_bytes=PAGE_MAIN_BYTES,
            oob_bytes=physical.page_bytes - PAGE_MAIN_BYTES,
            pages_per_block=PAGES_PER_BLOCK,
            block_count=PHYSICAL_PAGE_COUNT // PAGES_PER_BLOCK,
            raw_bytes=len(physical.raw),
        )
        selected_recipe = acquisition_recipe
    return save_backup_bytes(
        destination,
        physical.raw,
        geometry,
        target=spec.target,
        acquisition_recipe=selected_recipe,
    )


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
        if (
            not isinstance(name, str)
            or not name
            or name in {".", ".."}
            or Path(name).name != name
            or "\\" in name
            or "\x00" in name
        ):
            fail(f"device-data group {group} returned an unsafe {label} filename")
        if not isinstance(contents, bytes):
            fail(f"device-data group {group} returned non-byte {label} contents")
        result[name] = contents
    return result


def _normalize_groups(
    produced: Mapping[str, object],
    groups: Mapping[str, list[dict[str, Any]]],
) -> dict[str, PreparedGroup]:
    """Require complete declared groups and normalize their immutable files."""
    if set(produced) != set(groups):
        expected = ", ".join(sorted(groups))
        fail(f"device-data preparation returned an invalid group set; expected: {expected}")
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


def _extract_original_groups(
    spec: DeviceDataSpec, originals: dict[str, bytes]
) -> dict[str, PreparedGroup]:
    """Prepare named originals and preserve every admitted input byte in the result."""
    if spec.originals_parser is None:
        fail(f"target {spec.target} does not declare a named-original parser")
    parser = _load_originals_parser(spec.target, spec.originals_parser)
    try:
        result = parser.prepare_from_originals(MappingProxyType(originals))
    except ValueError as error:
        fail(f"device-data preparation failed: {error}")
    if not isinstance(result, DeviceDataPreparation):
        fail("device-data originals parser returned an invalid preparation")
    if not result.groups or not set(result.groups).issubset(spec.groups):
        fail("device-data originals parser returned undeclared or empty groups")
    normalized = _normalize_groups(
        result.groups, {name: spec.groups[name] for name in result.groups}
    )
    retained: dict[str, bytes] = {}
    for group in normalized.values():
        for name, contents in group.originals.items():
            if name not in originals or contents != originals[name]:
                fail("device-data originals parser did not preserve the admitted originals")
            retained[name] = contents
    if retained != originals:
        fail("device-data originals parser did not preserve the complete admitted original set")
    return normalized


def _extract_groups(spec: DeviceDataSpec, nand: PhysicalNand) -> dict[str, PreparedGroup]:
    """Run only the target's declared runtime-data parser."""
    if spec.parser is None:
        fail(f"target {spec.target} does not declare a physical NAND parser")
    parser = _load_device_data_parser(spec.target, spec.parser)
    try:
        result = parser.prepare_device_data(nand, audio=spec.audio)
    except ValueError as error:
        fail(f"device-data extraction failed: {error}")
    if not isinstance(result, DeviceDataPreparation):
        fail("device-data parser returned an invalid preparation")
    return _normalize_groups(result.groups, spec.groups)


def _publish_files(directory: Path, files: Mapping[str, bytes], name: str) -> None:
    """Write one complete parser output set into a new private directory."""
    destination = _require_directory(directory, name)
    for filename, contents in files.items():
        replace_file_atomically(destination / filename, contents, 0o600)


def materialize_device_data(
    staging: Path,
    groups: Mapping[str, PreparedGroup],
    declarations: Mapping[str, list[dict[str, Any]]],
) -> dict[str, tuple[FirmwareInput, ...]]:
    """Save prepared groups and reports, then admit every declared output byte."""
    prepared = _require_directory(staging / "groups", "device-data group directory")
    for group_name, group in groups.items():
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
    return capture_phone_data(declarations, staging)


def _write_receipt(  # noqa: PLR0913 -- source, geometry and prepared records have separate identities.
    staging: Path,
    *,
    source_kind: str,
    raw: bytes,
    groups: Mapping[str, PreparedGroup],
    admitted: Mapping[str, tuple[FirmwareInput, ...]],
    preparation_recipe: str,
    geometry: dict[str, Any] | None,
) -> None:
    """Record source and output identities without copying private source contents."""
    group_records: list[dict[str, Any]] = []
    for group_name in sorted(groups):
        group = groups[group_name]
        group_record: dict[str, Any] = {
            "name": group_name,
            "originals": [
                {"source": name, "size": len(contents), "sha256": sha256_bytes(contents)}
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
        "source": {"kind": source_kind, "size": len(raw), "sha256": sha256_bytes(raw)},
        "groups": group_records,
        "preparation_recipe": preparation_recipe,
        "geometry": geometry,
    }
    replace_file_atomically(staging / "receipt.json", canonical_json_bytes(record), 0o600)


def _require_destination(destination: Path, *, replace: bool) -> None:
    """Require a valid publication parent and the caller's explicit replacement policy."""
    if destination.parent.is_symlink() or not destination.parent.is_dir():
        fail(f"prepared phone data parent is missing or invalid: {destination.parent}")
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        fail(f"prepared phone data is invalid: {destination}")
    if destination.exists() and not replace:
        fail(f"prepared phone data already exists: {destination}")


def publish_phone_data(staging: Path, destination: Path, *, replace: bool = False) -> Path:
    """Install complete data under caller-owned locks, restoring old data on install error."""
    _require_destination(destination, replace=replace)
    previous: Path | None = None
    try:
        if destination.exists():
            previous = Path(
                tempfile.mkdtemp(prefix=f".{destination.name}-previous-", dir=destination.parent)
            )
            previous.rmdir()
            destination.replace(previous)
        try:
            staging.replace(destination)
        except OSError:
            if previous is not None:
                previous.replace(destination)
                previous = None
            raise
    except OSError as error:
        fail(f"prepared phone data cannot be installed: {error}")
    if previous is not None:
        shutil.rmtree(previous)
    return destination


def _materialize_and_publish(  # noqa: PLR0913 -- admitted source, output and host policy.
    spec: DeviceDataSpec,
    destination: Path,
    tools: DeviceDataTools,
    groups: Mapping[str, PreparedGroup],
    *,
    raw: bytes,
    source_kind: str,
    replace: bool,
    geometry: dict[str, Any] | None = None,
) -> Path:
    """Keep incomplete outputs private and expose them only after complete admission."""
    if os.name == "nt" and tools.protect_private is None:
        fail("prepared phone data requires the Windows private-directory policy")
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}-", dir=destination.parent))
    try:
        if tools.protect_private is not None:
            tools.protect_private(staging)
        selected = {name: spec.groups[name] for name in groups}
        if destination.is_dir():
            preserved = capture_phone_data(
                {
                    name: declarations
                    for name, declarations in spec.groups.items()
                    if name not in groups
                },
                destination,
                require_all=False,
            )
            if preserved:
                _require_directory(staging / "groups", "device-data group directory")
            for name, files in preserved.items():
                _publish_files(
                    staging / "groups" / name,
                    {firmware.source: firmware.contents for firmware in files},
                    f"preserved device-data group {name}",
                )
        admitted = materialize_device_data(staging, groups, selected)
        from .cache import preparation_identity  # noqa: PLC0415 -- receipt publication boundary.

        _write_receipt(
            staging,
            source_kind=source_kind,
            raw=raw,
            groups=groups,
            admitted=admitted,
            preparation_recipe=preparation_identity(spec, tools),
            geometry=geometry,
        )
        return publish_phone_data(staging, destination, replace=replace)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def prepare_from_nand(
    spec: DeviceDataSpec,
    backup: Path,
    destination: Path,
    tools: DeviceDataTools,
    *,
    replace: bool = False,
) -> Path:
    """Prepare one flat phone store while leaving the sole NAND backup unchanged."""
    _require_destination(destination, replace=replace)
    if not spec.groups:
        fail(f"device-data preparation is not supported for target {spec.target}")
    nand = read_physical_nand(spec, backup, tools)
    record = read_backup_record(backup, nand.raw)
    groups = _extract_groups(spec, nand)
    return _materialize_and_publish(
        spec,
        destination,
        tools,
        groups,
        raw=nand.raw,
        source_kind="physical-nand",
        replace=replace,
        geometry=None if record is None else asdict(record.geometry),
    )


def prepare_from_originals(
    spec: DeviceDataSpec,
    manifest: Path,
    destination: Path,
    tools: DeviceDataTools,
    *,
    replace: bool = False,
) -> Path:
    """Use the same admission and publication for the explicit named-original import."""
    _require_destination(destination, replace=replace)
    if not spec.groups:
        fail(f"device-data preparation is not supported for target {spec.target}")
    source = read_original_files(spec.target, manifest)
    groups = _extract_original_groups(spec, source.files)
    return _materialize_and_publish(
        spec,
        destination,
        tools,
        groups,
        raw=source.manifest,
        source_kind="original-files",
        replace=replace,
    )
