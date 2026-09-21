# SPDX-License-Identifier: GPL-2.0-only
"""Prepare all target-declared device data from one physical NAND image."""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.cli.build import build
from fplinux_cli.cli.runtime import run_target_noninteractive
from fplinux_cli.manifests.targets import load_target

from .common import ROOT, canonical_json_bytes, fail, replace_file_atomically, sha256_bytes
from .device_data import DeviceDataPreparation, PhysicalNand, PreparedGroup
from .firmware_inputs import (
    FirmwareInput,
    capture_device_data_generation,
    device_data_cache_directory,
)
from .nand_backup import backup_target_nand

if TYPE_CHECKING:
    from collections.abc import Mapping
    from types import ModuleType


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


def _load_device_data_parser(target: str, filename: str) -> ModuleType:
    """Load one target-owned parser through its normal source module boundary."""
    path = ROOT / "targets" / target / filename
    if path.is_symlink() or not path.is_file():
        fail(f"device-data parser is missing or invalid: {path}")
    name = f"fplinux_{target.replace('-', '_')}_device_data"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"device-data parser cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    if not callable(getattr(module, "prepare_device_data", None)):
        fail("device-data parser does not expose prepare_device_data(nand)")
    return module


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
    parser_filename: str,
    groups: Mapping[str, list[dict[str, Any]]],
    nand: PhysicalNand,
) -> dict[str, PreparedGroup]:
    """Run the target parser once and normalize every requested group."""
    parser = _load_device_data_parser(target, parser_filename)
    try:
        result = parser.prepare_device_data(nand)
    except ValueError as error:
        fail(f"device-data extraction failed: {error}")
    if not isinstance(result, DeviceDataPreparation) or set(result.groups) != set(groups):
        expected = ", ".join(sorted(groups))
        fail(f"device-data parser returned an invalid group set; expected: {expected}")

    normalized: dict[str, PreparedGroup] = {}
    for group_name in sorted(groups):
        prepared_group = result.groups[group_name]
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
        normalized[group_name] = PreparedGroup(originals=originals, prepared=prepared)
    return normalized


def _publish_files(directory: Path, files: Mapping[str, bytes], name: str) -> None:
    """Write one complete parser output set into a new private directory."""
    destination = _require_directory(directory, name)
    for filename, contents in files.items():
        replace_file_atomically(destination / filename, contents, 0o600)


def _materialize_groups(staging: Path, groups: Mapping[str, PreparedGroup]) -> None:
    """Save each group's originals and prepared bytes in a new generation."""
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


def _write_receipt(
    staging: Path,
    *,
    source_kind: str,
    raw: bytes,
    groups: Mapping[str, PreparedGroup],
    admitted: Mapping[str, tuple[FirmwareInput, ...]],
) -> None:
    """Record hashes and sizes without exposing source paths or private contents."""
    record = {
        "source": {
            "kind": source_kind,
            "size": len(raw),
            "sha256": sha256_bytes(raw),
        },
        "groups": [
            {
                "name": group_name,
                "originals": [
                    {
                        "source": name,
                        "size": len(contents),
                        "sha256": sha256_bytes(contents),
                    }
                    for name, contents in groups[group_name].originals.items()
                ],
                "prepared": [firmware.recipe_record() for firmware in admitted[group_name]],
            }
            for group_name in sorted(groups)
        ],
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
    parser_filename: str = device_data["parser"]
    raw_page_bytes: int = target_config["nand"]["raw_page_bytes"]
    cache = ROOT / ".cache"

    if from_dump is None:
        print("Device-data preparation: build the default RAM system.", flush=True)
        build(target, jobs, offline=offline)
        print("Device-data preparation: load the default RAM system.", flush=True)
        print(
            "Device-data preparation: connect the powered-off phone only when the loader asks.",
            flush=True,
        )
        run_target_noninteractive(target, profile=None)
        source_kind = "live-nand"
    else:
        print(f"Device-data preparation: use saved NAND dump {from_dump}.", flush=True)
        source_kind = "saved-dump"

    staging = _create_staging_generation(cache, target)
    selected = False
    try:
        source_directory = _require_directory(
            staging / "source",
            "device-data source directory",
        )
        source = source_directory / "nand.bin"
        if from_dump is None:
            print(
                "Device-data preparation: dump the complete NAND through the read-only reader.",
                flush=True,
            )
            backup_target_nand(target, source)
            raw = _read_dump(source)
        else:
            raw = _read_dump(from_dump)
            replace_file_atomically(source, raw, 0o600)

        print("Device-data preparation: extract and validate all declared groups.", flush=True)
        try:
            nand = PhysicalNand.from_dump(raw, page_bytes=raw_page_bytes)
        except ValueError as error:
            fail(f"device-data extraction failed: {error}")
        extracted = _extract_groups(target, parser_filename, declarations, nand)
        _materialize_groups(staging, extracted)
        admitted = capture_device_data_generation(
            declarations,
            staging,
            path_field="source",
            require_all=True,
        )
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

    print(f"Device data is ready in {generation}.")
    print("Next:")
    print(f"  ./fplinux build {target}")
    print(f"  ./fplinux run {target}")
