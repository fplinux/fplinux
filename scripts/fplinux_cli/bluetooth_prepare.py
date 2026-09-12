# SPDX-License-Identifier: GPL-2.0-only
"""Prepare target-owned Bluetooth firmware from a physical NAND backup."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .commands import build, run_target_noninteractive
from .common import ROOT, fail, replace_file_atomically, sha256_bytes
from .config import load_target
from .firmware_inputs import (
    FirmwareInput,
    capture_external_firmware_inputs,
    external_firmware_directory,
)
from .nand_backup import backup_target_nand

if TYPE_CHECKING:
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


def _firmware_directory(cache: Path, target: str) -> Path:
    """Create the fixed private firmware directory chain below an existing cache root."""
    if cache.is_symlink() or not cache.is_dir():
        fail(f"cache directory is missing or invalid: {cache}")
    _require_directory(cache / "firmware", "firmware cache directory")
    return _require_directory(
        external_firmware_directory(cache, target),
        f"firmware directory for {target}",
    )


def _source_run_directory(cache: Path, target: str) -> Path:
    """Allocate one private, never-reused provenance directory."""
    firmware = _firmware_directory(cache, target)
    sources = _require_directory(firmware / "sources", "firmware source directory")
    try:
        return Path(tempfile.mkdtemp(prefix="run-", dir=sources))
    except OSError as error:
        fail(f"firmware source run cannot be created: {error}")


def _read_dump(path: Path) -> bytes:
    """Read one regular NAND backup without changing or replacing it."""
    if path.is_symlink() or not path.is_file():
        fail(f"NAND backup is missing or invalid: {path}")
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"NAND backup cannot be read: {path}: {error}")


def _load_firmware_parser(target: str, filename: str) -> ModuleType:
    """Load the target-owned parser through its normal source module boundary."""
    path = ROOT / "targets" / target / filename
    if path.is_symlink() or not path.is_file():
        fail(f"Bluetooth firmware parser is missing or invalid: {path}")
    name = f"fplinux_{target.replace('-', '_')}_bluetooth_firmware"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"Bluetooth firmware parser cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    if not callable(getattr(module, "prepare_firmware", None)):
        fail("Bluetooth firmware parser does not expose prepare_firmware(raw)")
    return module


def _preparation_config(target: str) -> tuple[str, list[dict[str, Any]]]:
    """Return the parser and firmware declarations shared by both boot profiles."""
    config = load_target(target)
    if "bluetooth" not in config:
        fail(f"Bluetooth firmware preparation is not supported for target {target}")
    declarations = config["rootfs"]["firmware"]
    if not isinstance(declarations, list) or not declarations:
        fail(f"target {target} has no declared Bluetooth firmware inputs")
    return str(config["bluetooth"]["parser"]), declarations


def _canonical_files(
    value: object,
    expected_names: tuple[str, ...],
    label: str,
) -> dict[str, bytes]:
    """Require one exact parser output set containing only immutable byte strings."""
    if not isinstance(value, dict) or set(value) != set(expected_names):
        expected = ", ".join(expected_names)
        fail(f"Bluetooth firmware parser returned an invalid {label} set; expected: {expected}")
    if any(not isinstance(contents, bytes) for contents in value.values()):
        fail(f"Bluetooth firmware parser returned non-byte {label} contents")
    return {name: value[name] for name in expected_names}


def _extract_firmware(
    target: str,
    parser_filename: str,
    raw: bytes,
    expected_names: tuple[str, ...],
) -> tuple[dict[str, bytes], dict[str, bytes]]:
    """Run the target parser and normalize its two exact output sets."""
    parser = _load_firmware_parser(target, parser_filename)
    try:
        result = parser.prepare_firmware(raw)
    except ValueError as error:
        fail(f"Bluetooth firmware extraction failed: {error}")
    originals = _canonical_files(getattr(result, "originals", None), expected_names, "original")
    prepared = _canonical_files(getattr(result, "prepared", None), expected_names, "prepared")
    return originals, prepared


def _publish_originals(directory: Path, originals: dict[str, bytes]) -> None:
    """Preserve unchanged fitted files in their unique source-run directory."""
    destination = _require_directory(directory / "originals", "original firmware directory")
    for name, contents in originals.items():
        path = destination / name
        if path.exists() or path.is_symlink():
            fail(f"original firmware destination already exists: {path}")
        replace_file_atomically(path, contents, 0o600)


def _admit_prepared_firmware(
    target: str,
    declarations: list[dict[str, Any]],
    prepared: dict[str, bytes],
    source_run: Path,
) -> tuple[FirmwareInput, ...]:
    """Validate all prepared bytes before any current build input is replaced."""
    with tempfile.TemporaryDirectory(prefix=".admission-", dir=source_run) as temporary:
        admission_cache = Path(temporary)
        staging = _firmware_directory(admission_cache, target)
        for name, contents in prepared.items():
            replace_file_atomically(staging / name, contents, 0o600)
        return capture_external_firmware_inputs(target, declarations, admission_cache)


def _publish_prepared_firmware(
    target: str,
    declarations: list[dict[str, Any]],
    admitted: tuple[FirmwareInput, ...],
    cache: Path,
) -> tuple[FirmwareInput, ...]:
    """Replace only the four admitted current inputs, then verify their published bytes."""
    destination = _firmware_directory(cache, target)
    for firmware in admitted:
        path = destination / firmware.source
        if path.is_symlink() or (path.exists() and not path.is_file()):
            fail(f"firmware destination is not a regular file: {path}")
    for firmware in admitted:
        replace_file_atomically(destination / firmware.source, firmware.contents, 0o600)
    return capture_external_firmware_inputs(target, declarations, cache)


def _write_receipt(
    source_run: Path,
    source: Path,
    raw: bytes,
    originals: dict[str, bytes],
    prepared: tuple[FirmwareInput, ...],
) -> None:
    """Record local provenance without copying the source dump or private bytes."""
    record = {
        "source": str(source.absolute()),
        "source_sha256": sha256_bytes(raw),
        "originals": [
            {"source": name, "size": len(contents), "sha256": sha256_bytes(contents)}
            for name, contents in originals.items()
        ],
        "prepared": [firmware.recipe_record() for firmware in prepared],
    }
    contents = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode()
    replace_file_atomically(source_run / "receipt.json", contents, 0o600)


def prepare_bluetooth(
    target: str,
    *,
    from_dump: Path | None,
    jobs: int,
    offline: bool,
) -> None:
    """Prepare all Bluetooth build inputs from a live or previously saved NAND dump."""
    parser_filename, declarations = _preparation_config(target)
    expected_names = tuple(str(item["source"]) for item in declarations)
    cache = ROOT / ".cache"

    if from_dump is None:
        print("Bluetooth preparation: build the default RAM system.", flush=True)
        build(target, jobs, offline=offline)
        print("Bluetooth preparation: load the default RAM system.", flush=True)
        print(
            "Bluetooth preparation: connect the powered-off phone only when the loader asks.",
            flush=True,
        )
        run_target_noninteractive(target, profile=None)
        source_run = _source_run_directory(cache, target)
        source = source_run / "nand.bin"
        print(
            "Bluetooth preparation: dump the complete NAND through the read-only reader.",
            flush=True,
        )
        backup_target_nand(target, source)
    else:
        source = from_dump
        source_run = _source_run_directory(cache, target)
        print(f"Bluetooth preparation: use saved NAND dump {source}.", flush=True)

    raw = _read_dump(source)
    print(
        "Bluetooth preparation: extract and validate the four fitted firmware files.", flush=True
    )
    originals, prepared = _extract_firmware(target, parser_filename, raw, expected_names)
    _publish_originals(source_run, originals)
    admitted = _admit_prepared_firmware(target, declarations, prepared, source_run)
    published = _publish_prepared_firmware(target, declarations, admitted, cache)
    _write_receipt(source_run, source, raw, originals, published)

    print(f"Bluetooth firmware is ready in {external_firmware_directory(cache, target)}.")
    print("Next:")
    print(f"  ./fplinux build {target}")
    print(f"  ./fplinux run {target}")
