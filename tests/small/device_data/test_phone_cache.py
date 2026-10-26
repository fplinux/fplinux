# SPDX-License-Identifier: GPL-2.0-only
"""Admit small raw and prepared records by their actual source and output dependencies."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.device_data.cache import current_backup, prepared_data_current
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class PhoneCache:
    """Own independently described raw pages, prepared bytes and their input receipts."""

    backup: Path
    phone: Path
    spec: DeviceDataSpec
    nand: dict[str, Any]


@pytest.fixture
def phone_cache(tmp_path: Path) -> PhoneCache:
    """Keep receipt admission independent from the physical extraction implementation."""
    raw = b"0123456789abcdef"
    backup = tmp_path / "nand.bin"
    backup.write_bytes(raw)
    geometry = {
        "id_bytes": "1234",
        "chip": "controlled chip",
        "page_main_bytes": 1,
        "oob_bytes": 1,
        "pages_per_block": 4,
        "block_count": 2,
        "raw_bytes": 16,
    }
    backup.with_name("nand.bin.json").write_text(
        json.dumps(
            {
                **geometry,
                "sha256": hashlib.sha256(raw).hexdigest(),
                "target": "phone",
                "acquisition_recipe": "a" * 64,
            }
        ),
        encoding="utf-8",
    )
    phone = tmp_path / "phone"
    group = phone / "groups/bluetooth"
    group.mkdir(parents=True)
    (group / "radio.bin").write_bytes(b"radio")
    (phone / "receipt.json").write_text(
        json.dumps(
            {
                "source": {
                    "kind": "physical-nand",
                    "size": 16,
                    "sha256": hashlib.sha256(raw).hexdigest(),
                },
                "geometry": geometry,
                "preparation_recipe": (
                    "779d948785424ee189115b8a69823c010d3443b4b5836385b0884f5dcb5168cd"
                ),
                "groups": [
                    {
                        "name": "bluetooth",
                        "originals": [],
                        "prepared": [
                            {
                                "source": "radio.bin",
                                "destination": "chip/radio.bin",
                                "size": 5,
                                "sha256": hashlib.sha256(b"radio").hexdigest(),
                            }
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    nand = {"id": 0x3412, "raw_page_bytes": 2, "acquisition_recipe": "a" * 64}
    spec = DeviceDataSpec(
        target="phone",
        parser=None,
        preparation_recipe="b" * 64,
        groups={
            "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}]
        },
        audio=AudioProfileContext(
            machine_compatible=b"test,phone",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        nand=nand,
    )
    return PhoneCache(backup, phone, spec, nand)


def test_matching_raw_and_prepared_receipts_admit_the_actual_bytes(
    phone_cache: PhoneCache,
) -> None:
    cache = phone_cache

    record = current_backup(cache.backup, target="phone", nand=cache.nand)

    assert record is not None
    assert record.geometry.raw_bytes == 16
    assert record.sha256 == hashlib.sha256(b"0123456789abcdef").hexdigest()
    assert prepared_data_current(cache.spec, DeviceDataTools(), cache.phone, record)


def test_changed_acquisition_recipe_rejects_the_saved_raw_backup(phone_cache: PhoneCache) -> None:
    cache = phone_cache

    record = current_backup(
        cache.backup, target="phone", nand={**cache.nand, "acquisition_recipe": "c" * 64}
    )

    assert record is None


def test_changed_parser_recipe_rejects_prepared_data_and_retains_raw_hit(
    phone_cache: PhoneCache,
) -> None:
    cache = phone_cache
    record = current_backup(cache.backup, target="phone", nand=cache.nand)
    assert record is not None

    changed_spec = replace(cache.spec, preparation_recipe="c" * 64)

    assert not prepared_data_current(changed_spec, DeviceDataTools(), cache.phone, record)
    assert current_backup(cache.backup, target="phone", nand=cache.nand) == record


def test_changed_native_decoder_bytes_reject_prepared_data_and_retain_raw_hit(
    phone_cache: PhoneCache,
) -> None:
    """Check decoder compatibility by file bytes without executing NAND correction."""
    cache = phone_cache
    record = current_backup(cache.backup, target="phone", nand=cache.nand)
    assert record is not None
    native_tools = cache.phone.parent / "native-tools"
    native_tools.mkdir()
    decoder = native_tools / "ums9117-nand-ecc"
    decoder.write_bytes(b"native decoder A")
    spec = replace(cache.spec, nand={**cache.nand, "ecc_format": "ums9117-bch12"})
    tools = DeviceDataTools(host_tools=native_tools)
    receipt_path = cache.phone / "receipt.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    identity_document = json.dumps(
        {
            "implementation": "b" * 64,
            "native_ecc": hashlib.sha256(b"native decoder A").hexdigest(),
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    receipt["preparation_recipe"] = hashlib.sha256(
        (identity_document + "\n").encode("utf-8")
    ).hexdigest()
    receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
    assert prepared_data_current(spec, tools, cache.phone, record)

    decoder.write_bytes(b"native decoder B")

    assert not prepared_data_current(spec, tools, cache.phone, record)
    assert current_backup(cache.backup, target="phone", nand=cache.nand) == record


def test_changed_prepared_bytes_reject_the_previous_output_receipt(
    phone_cache: PhoneCache,
) -> None:
    cache = phone_cache
    record = current_backup(cache.backup, target="phone", nand=cache.nand)
    assert record is not None
    (cache.phone / "groups/bluetooth/radio.bin").write_bytes(b"other")

    assert not prepared_data_current(cache.spec, DeviceDataTools(), cache.phone, record)


@pytest.mark.parametrize("receipt_kind", ["backup", "prepared"])
@pytest.mark.parametrize("receipt_state", ["missing", "unknown"])
def test_missing_or_unknown_receipt_is_a_cache_miss(
    phone_cache: PhoneCache, receipt_kind: str, receipt_state: str
) -> None:
    cache = phone_cache
    record = current_backup(cache.backup, target="phone", nand=cache.nand)
    assert record is not None
    receipt = (
        cache.backup.with_name("nand.bin.json")
        if receipt_kind == "backup"
        else cache.phone / "receipt.json"
    )
    if receipt_state == "missing":
        receipt.unlink()
    else:
        receipt.write_text('{"unrecognized": true}\n', encoding="utf-8")

    if receipt_kind == "backup":
        assert current_backup(cache.backup, target="phone", nand=cache.nand) is None
    else:
        assert not prepared_data_current(cache.spec, DeviceDataTools(), cache.phone, record)
