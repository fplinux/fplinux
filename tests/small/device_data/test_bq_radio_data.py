# SPDX-License-Identifier: GPL-2.0-only
"""Host format checks for external-radio calibration and saved Wi-Fi addresses."""

from __future__ import annotations

import importlib.util
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.common import ROOT

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture
def radio_parser() -> ModuleType:
    """Load the target's normal parser without reading any fitted device data."""
    path = ROOT / "targets/bq-2830l-tank-connect-max/bq2830l_device_data.py"
    spec = importlib.util.spec_from_file_location("test_bq_radio_parser", path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def radio_records() -> dict[int, bytes]:
    """Create synthetic version-two records with recognizable opaque payloads."""
    configuration = b"\x02" + b"C" * 615 + b"unused configuration tail".ljust(272, b"T")
    calibration = b"\x02\x01" + b"K" * 3290
    return {409: configuration, 417: calibration}


def test_calibration_retains_only_the_cp_configuration_prefix_and_full_record(
    radio_parser: ModuleType,
) -> None:
    """The CP input retains opaque bytes and excludes the unrelated configuration tail."""
    downloaded = radio_records()
    protected = downloaded.copy()

    result = radio_parser.prepare_radio_calibration(downloaded, protected)

    assert result == b"\x02" + b"C" * 615 + b"\x02\x01" + b"K" * 3290
    assert len(result) == 3908
    assert downloaded == protected == radio_records()


@pytest.mark.parametrize("identifier", [409, 417])
def test_disagreeing_fixed_radio_records_are_rejected(
    radio_parser: ModuleType, identifier: int
) -> None:
    """A mismatched opaque byte cannot be silently selected from either fixed copy."""
    downloaded = radio_records()
    protected = downloaded.copy()
    protected[identifier] = protected[identifier][:-1] + b"!"

    with pytest.raises(ValueError, match="differs between fixed copies"):
        radio_parser.prepare_radio_calibration(downloaded, protected)


@pytest.mark.parametrize(
    ("identifier", "offset", "value", "error"),
    [
        (409, 0, 3, "unsupported radio configuration"),
        (417, 0, 3, "unsupported radio configuration"),
        (417, 1, 0, "not already calibrated"),
    ],
)
def test_radio_format_and_uncalibrated_records_are_rejected(
    radio_parser: ModuleType, identifier: int, offset: int, value: int, error: str
) -> None:
    """Unsupported formats and a record requesting fresh calibration are not admitted."""
    downloaded = radio_records()
    record = bytearray(downloaded[identifier])
    record[offset] = value
    downloaded[identifier] = bytes(record)

    with pytest.raises(ValueError, match=error):
        radio_parser.prepare_radio_calibration(downloaded, downloaded.copy())


@pytest.mark.parametrize("identifier", [409, 417])
def test_incomplete_radio_record_is_rejected(radio_parser: ModuleType, identifier: int) -> None:
    """Truncated input cannot produce a shorter CP transfer with otherwise valid flags."""
    downloaded = radio_records()
    downloaded[identifier] = downloaded[identifier][:-1]

    with pytest.raises(ValueError, match="missing or has the wrong size"):
        radio_parser.prepare_radio_calibration(downloaded, downloaded.copy())


def wifi_address_record() -> bytes:
    """Return a synthetic ordinary NV416/6 record with literal wire checksums.

    The header's words sum to 0x01a7; the address's words sum to 0x9968.
    Their one's-complement checksums are independently fixed here.
    """
    return bytes.fromhex("58fe9766a001060001000000021122334455").ljust(32, b"\xff")


@pytest.mark.parametrize("prefix_slots", [0, 1, 65])
def test_saved_wifi_address_accepts_redundant_agreeing_records(
    radio_parser: ModuleType, prefix_slots: int
) -> None:
    """The same saved address is retained independently of physical slot position."""
    record = wifi_address_record()
    partition = b"\xff" * (32 * prefix_slots) + record + b"\xff" * 64 + record

    assert radio_parser.prepare_radio_mac(partition) == b"\x02\x11\x22\x33\x44\x55"


@pytest.mark.parametrize(
    "unusable",
    [
        pytest.param("58feffffa001060001000000000000000000", id="zero-address"),
        pytest.param("58fe9666a001060001000000031122334455", id="multicast-address"),
        pytest.param("59fe9755a001060001000000021122334466", id="bad-header-checksum"),
        pytest.param("58fe9655a001060001000000021122334466", id="bad-data-checksum"),
        pytest.param("57fe2055a00107000100000002112233446677", id="wrong-record-size"),
        pytest.param("57fe9755a001060002000000021122334466", id="nonordinary-record"),
    ],
)
def test_saved_wifi_address_ignores_unusable_copies(
    radio_parser: ModuleType, unusable: str
) -> None:
    """Unsuitable or unverified historical data cannot replace a usable saved value."""
    rejected_record = bytes.fromhex(unusable).ljust(32, b"\xff")
    partition = rejected_record + wifi_address_record() + rejected_record

    assert radio_parser.prepare_radio_mac(partition) == b"\x02\x11\x22\x33\x44\x55"


@pytest.mark.parametrize(
    "partition",
    [
        pytest.param(b"\xff" * 96, id="absent-record"),
        pytest.param(
            bytes.fromhex("58feffffa001060001000000000000000000").ljust(32, b"\xff"),
            id="zero-address",
        ),
        pytest.param(
            bytes.fromhex("58fe9666a001060001000000031122334455").ljust(32, b"\xff"),
            id="multicast-address",
        ),
        pytest.param(wifi_address_record()[:17], id="truncated-record"),
        pytest.param(b"\xff" + wifi_address_record(), id="outside-short-record-slot"),
    ],
)
def test_saved_wifi_address_rejects_missing_admitted_value(
    radio_parser: ModuleType, partition: bytes
) -> None:
    """No fallback address is synthesized when no usable saved record is admitted."""
    with pytest.raises(ValueError, match="no checksum-valid nonzero unicast"):
        radio_parser.prepare_radio_mac(partition)


@pytest.mark.parametrize("reverse", [False, True])
def test_saved_wifi_address_rejects_distinct_valid_values(
    radio_parser: ModuleType, *, reverse: bool
) -> None:
    """Physical order cannot choose between two checksum-valid saved addresses."""
    first = wifi_address_record()
    second = bytes.fromhex("58fe9755a001060001000000021122334466").ljust(32, b"\xff")
    partition = second + first if reverse else first + second

    with pytest.raises(ValueError, match="ambiguous RunningNV Wi-Fi addresses"):
        radio_parser.prepare_radio_mac(partition)
