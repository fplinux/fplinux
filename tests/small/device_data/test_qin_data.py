# SPDX-License-Identifier: GPL-2.0-only
"""Qin radio template encoding with independent literal payload expectations."""

from __future__ import annotations

import importlib.util
import sys
from typing import TYPE_CHECKING

import pytest

from tests import ROOT

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture
def qin_parser(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    """Load the target's actual encoder through its normal module boundary."""
    path = ROOT / "targets/xiaomi-qin-1s-plus/qin1splus_device_data.py"
    name = "test_qin1splus_device_data"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        message = "Qin device-data encoder is unavailable"
        raise ImportError(message)
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, name, module)
    spec.loader.exec_module(module)
    return module


def test_config_field_widths_padding_and_partial_arrays(qin_parser: ModuleType) -> None:
    """The corrected reserved key and short arrays preserve their independent extents."""
    payload = qin_parser.encode_rf_template(
        b"conf_version=1\ninit_rf_regs=0x12345678,2\n"
        b"reserved_w16_regs=0x5678\nreserved_w32_regs=0x1234\n",
        kind="config",
    )

    assert len(payload) == 616
    assert payload[:4] == b"\x01\0\0\0"
    assert payload[0x31:0x34] == b"\0\0\0"
    assert payload[0x68:0x70] == b"\x78\x56\x34\x12\x02\0\0\0"
    assert payload[0x70:0xA8] == bytes(56)
    assert payload[0xAA:0xAC] == b"\x78\x56"
    assert payload[0xC0:0xC2] == b"\x34\x12"


def test_cali_conversion_uses_signed64_then_low32_and_member_width(qin_parser: ModuleType) -> None:
    """Successful conversion precedes stores, including unsigned negative bit patterns."""
    payload = qin_parser.encode_rf_template(
        b"cali_version=01\nconfig_reserved=-128,0xff,257\n"
        b"rf_txiq_c12=0xffffffe6\nrf_txiq_dc=0xffffbffb\n"
        b"dcoc_cali_code=-1,0x10001\n"
        b"txpower_psat_temperature=-9223372036854775808\n"
        b"tpc_cfg=0x100000001,9223372036854775807\n",
        kind="cali",
    )

    assert len(payload) == 3292
    assert payload[:4] == b"\x01\0\0\0"
    assert payload[8:12] == bytes.fromhex("80 ff 01 00")
    assert payload[0x10:0x14] == bytes.fromhex("ff ff 01 00")
    assert payload[0x48:0x4C] == bytes.fromhex("e6 ff ff ff")
    assert payload[0x50:0x54] == bytes.fromhex("fb bf ff ff")
    assert payload[0x74:0x78] == bytes(4)
    assert payload[0xC04:0xC0C] == bytes.fromhex("01 00 00 00 ff ff ff ff")


def test_known_non_rf_section_cannot_change_rf_payload(qin_parser: ModuleType) -> None:
    """The case-sensitive zero mismatch and section 6 fields remain outside RF bytes."""
    plain = qin_parser.encode_rf_template(b"conf_version=1\n", kind="config")
    with_non_rf = qin_parser.encode_rf_template(
        b"conf_version=1\n[SETCTION 2]\nRF_init_num=0\n"
        b"[SETCTION 6]\npskey_cmd=0xfca0\ndevice_addr=1,2,3,4,5,6\n",
        kind="config",
    )

    assert with_non_rf == plain
    assert with_non_rf[0x64] == 0


@pytest.mark.parametrize(
    ("data", "error"),
    [
        (b"conf_version=1\nchannel_num=09\n", "integer spelling"),
        (b"conf_version=1\nchannel_num=1,\n", "integer spelling"),
        (b"conf_version=1\nchannel_num=9223372036854775808\n", "64-bit"),
        (b"conf_version=1\nchannel_num=-9223372036854775809\n", "64-bit"),
        (b"conf_version=1\nchannel_num 3\n", "assignment"),
        (b"conf_version=1\nconf_version=1\n", "duplicate"),
        (b"conf_version=1\nunknown=1\n", "unknown"),
        (b"conf_version=1\nchannel_range=1,2,3,4,5,6,7\n", "element count"),
        (b"conf_version=1\n[SETCTION 2]\nRF_init_num=1\n", "original zero"),
        (b"conf_version=1\npskey_cmd=1\n", "outside section 6"),
        (b"conf_version=1", "unterminated"),
        (b"conf_version=1\n\0\n", "NUL"),
        (b"channel_num=3\n", "missing radio version"),
        (b"conf_version=0\n", "all-zero"),
    ],
)
def test_malformed_ini_never_produces_a_template(
    qin_parser: ModuleType, data: bytes, error: str
) -> None:
    """Format failures are explicit; there is no stale numeric value or fallback field."""
    with pytest.raises(ValueError, match=error):
        qin_parser.encode_rf_template(data, kind="config")


def test_preparation_retains_originals_and_both_variant_pairs(qin_parser: ModuleType) -> None:
    """The producer prepares every requested image without selecting hardware identity."""
    firmware = b"\x5a" * 1540096
    originals = {
        "wcnmodem-firmware.bin": firmware,
        "system-connectivity_configure.ini": b"conf_version=1\n",
        "system-connectivity_calibration.ini": b"cali_version=1\n",
        "wcn-connectivity_configure.ini": b"conf_version=2\n",
        "wcn-connectivity_calibration.ini": b"cali_version=2\n",
    }
    expected_originals = dict(originals)

    group = qin_parser.prepare_from_originals(originals).groups["wcn-firmware"]

    assert originals == expected_originals
    assert group.originals == expected_originals
    assert group.originals is not originals
    assert group.prepared == {
        "wcnmodem.bin": firmware,
        "system-config.bin": b"\x01" + bytes(615),
        "system-cali.bin": b"\x01" + bytes(3291),
        "wcn-config.bin": b"\x02" + bytes(615),
        "wcn-cali.bin": b"\x02" + bytes(3291),
    }


@pytest.mark.parametrize("image_size", [0, 1540095, 1540097, 10485760])
def test_preparation_rejects_a_full_partition_or_wrong_image_length(
    qin_parser: ModuleType, image_size: int
) -> None:
    """The bounded image contract cannot silently trim or accept a complete partition."""
    originals = {
        "wcnmodem-firmware.bin": bytes(image_size),
        "system-connectivity_configure.ini": b"conf_version=1\n",
        "system-connectivity_calibration.ini": b"cali_version=1\n",
        "wcn-connectivity_configure.ini": b"conf_version=1\n",
        "wcn-connectivity_calibration.ini": b"cali_version=1\n",
    }

    with pytest.raises(ValueError, match="exactly 1540096"):
        qin_parser.prepare_from_originals(originals)
