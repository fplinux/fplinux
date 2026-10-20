# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Encode Qin 1s+ radio templates from separately admitted original files."""

from __future__ import annotations

import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Literal

from fplinux_cli.device_data.formats import DeviceDataPreparation, PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

_SIZES = {"config": 616, "cali": 3292}
_NUMBER = re.compile(r"-?(?:0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)\Z")
_ASSIGNMENT = re.compile(r"([A-Za-z_][A-Za-z_0-9]*)\s*=\s*(.*)\Z")
_SECTION = re.compile(r"\[SETCTION [0-9]+\]\Z")
_ORIGINAL_NAMES = frozenset(
    {
        "wcnmodem-firmware.bin",
        "system-connectivity_configure.ini",
        "system-connectivity_calibration.ini",
        "wcn-connectivity_configure.ini",
        "wcn-connectivity_calibration.ini",
    }
)
_CONFIG_NON_RF_COUNTS = {
    "RF_init_num": 1,
    "pskey_cmd": 1,
    "g_dbg_source_sink_syn_test_data": 1,
    "g_sys_sleep_in_standby_supported": 1,
    "g_sys_sleep_master_supported": 1,
    "g_sys_sleep_slave_supported": 1,
    "default_ahb_clk": 1,
    "device_class": 1,
    "win_ext": 1,
    "g_aGainValue": 6,
    "g_aPowerValue": 5,
    "g_aBRChannelpwrvalue": 8,
    "g_aEDRChannelpwrvalue": 8,
    "g_aLEPowerControlFlag": 1,
    "g_aLEChannelpwrvalue": 8,
    "g_central_or_peripheral": 1,
    "feature_set": 16,
    "device_addr": 6,
    "g_sys_sco_transmit_mode": 1,
    "g_sys_uart0_communication_supported": 1,
    "edr_tx_edr_delay": 1,
    "edr_rx_edr_delay": 1,
    "g_wbs_nv_117": 1,
    "is_wdg_supported": 1,
    "share_memo_rx_base_addr": 1,
    "g_wbs_nv_118": 1,
    "g_nbv_nv_117": 1,
    "share_memo_tx_packet_num_addr": 1,
    "share_memo_tx_data_base_addr": 1,
    "g_PrintLevel": 1,
    "share_memo_tx_block_length": 1,
    "share_memo_rx_block_length": 1,
    "share_memo_tx_water_mark": 1,
    "g_nbv_nv_118": 1,
    "uart_rx_watermark": 1,
    "uart_flow_control_thld": 1,
    "comp_id": 1,
    "pcm_clk_divd": 1,
    "br_edr_diff_reserved": 1,
    "bt_reserved": 8,
}


@dataclass(frozen=True)
class _Field:
    """A field's exact byte extent in one zero-initialized radio template."""

    offset: int
    width: int
    count: int


def _load_fields(kind: str) -> dict[str, _Field]:
    """Read the target-owned layout resource for one payload type."""
    path = Path(__file__).parent / "device-data/rf-fields.tsv"
    fields = {}
    with path.open(encoding="ascii", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if row["kind"] == kind:
                fields[row["key"]] = _Field(
                    offset=int(row["offset"], 0),
                    width=int(row["width"]),
                    count=int(row["count"]),
                )
    return fields


def _parse_number(token: str) -> int:
    """Require a successful signed 64-bit base-zero conversion."""
    if _NUMBER.fullmatch(token) is None:
        message = "invalid radio integer spelling"
        raise ValueError(message)
    magnitude = token.removeprefix("-")
    base = 16 if magnitude.lower().startswith("0x") else 8 if magnitude.startswith("0") else 10
    value = int(token, base)
    if not -(1 << 63) <= value < 1 << 63:
        message = "radio integer exceeds signed 64-bit conversion range"
        raise ValueError(message)
    return value


def encode_rf_template(data: bytes, *, kind: Literal["config", "cali"]) -> bytes:
    """Encode admitted INI bytes, retaining zero padding and low-width stores.

    This format encoder does not admit an original file or select a chip revision.
    The caller must establish the input identity before preparing device data.
    """
    text = data.decode("ascii")
    if "\0" in text:
        message = "NUL in radio INI"
        raise ValueError(message)
    if not text.endswith(("\r", "\n")):
        message = "unterminated final radio INI line"
        raise ValueError(message)
    fields = _load_fields(kind)
    payload = bytearray(_SIZES[kind])
    seen: set[str] = set()
    section = ""
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.partition("#")[0].strip()
        if not line:
            continue
        if _SECTION.fullmatch(line):
            section = line
            continue
        assignment = _ASSIGNMENT.fullmatch(line)
        if assignment is None:
            raise ValueError(f"radio INI line {line_number}: malformed assignment")
        key, value_text = assignment.groups()
        if key in seen:
            raise ValueError(f"radio INI line {line_number}: duplicate key {key}")
        seen.add(key)
        values = [_parse_number(token.strip()) for token in value_text.split(",")]
        field = fields.get(key)
        if field is None:
            count = _CONFIG_NON_RF_COUNTS.get(key) if kind == "config" else None
            if count is None:
                raise ValueError(f"radio INI line {line_number}: unknown {kind} key {key}")
            if len(values) != count or any(not 0 <= value <= 0xFFFFFFFF for value in values):
                raise ValueError(f"radio INI line {line_number}: invalid non-RF field {key}")
            if key == "RF_init_num":
                if values != [0] or section != "[SETCTION 2]":
                    message = "RF_init_num requires section 2 and the original zero value"
                    raise ValueError(message)
            elif section != "[SETCTION 6]":
                raise ValueError(f"radio INI line {line_number}: non-RF field outside section 6")
            continue
        if not 1 <= len(values) <= field.count:
            raise ValueError(f"radio INI line {line_number}: {key} has invalid element count")
        mask = (1 << (8 * field.width)) - 1
        for index, value in enumerate(values):
            # Conversion yields a signed 64-bit value; stores retain low32 and
            # then the destination width, including unsigned member bit patterns.
            stored = (value & 0xFFFFFFFF) & mask
            offset = field.offset + index * field.width
            payload[offset : offset + field.width] = stored.to_bytes(field.width, "little")
    version = "conf_version" if kind == "config" else "cali_version"
    if version not in seen or not any(payload):
        message = "missing radio version or all-zero payload"
        raise ValueError(message)
    return bytes(payload)


def prepare_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Prepare both Qin RF pairs from an independently admitted original set.

    Inputs are an already bounded firmware image and the four complete INI files.
    Partition/container extraction and original identity admission belong to the
    acquisition boundary. No physical-NAND parser is provided for this target.
    """
    if set(originals) != _ORIGINAL_NAMES:
        message = "Qin radio preparation requires the image and both original RF pairs"
        raise ValueError(message)
    firmware = originals["wcnmodem-firmware.bin"]
    if len(firmware) != 1540096:
        message = "Qin radio image must contain exactly 1540096 bytes"
        raise ValueError(message)
    prepared = {"wcnmodem.bin": firmware}
    for label in ("system", "wcn"):
        prepared[f"{label}-config.bin"] = encode_rf_template(
            originals[f"{label}-connectivity_configure.ini"], kind="config"
        )
        prepared[f"{label}-cali.bin"] = encode_rf_template(
            originals[f"{label}-connectivity_calibration.ini"], kind="cali"
        )
    return DeviceDataPreparation(
        groups={"wcn-firmware": PreparedGroup(originals=dict(originals), prepared=prepared)}
    )
