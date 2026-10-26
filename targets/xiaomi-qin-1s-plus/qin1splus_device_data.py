# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare Qin 1s+ device settings from separately admitted original files."""

from __future__ import annotations

import csv
import re
import struct
import xml.etree.ElementTree as ET
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
_AUDIO_ORIGINAL_NAMES = frozenset(
    {"audio_para", "audio_hw.xml", "tiny_hw.xml", "codec_pga.xml", "vbc_eq"}
)
_AUDIO_MAGIC = b"FPLQINAU"
_AUDIO_RATE = 44100
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


def _prepare_radio_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Prepare both Qin RF pairs from an independently admitted original set.

    Inputs are an already bounded firmware image and the four complete INI files.
    Partition/container extraction and original identity admission belong to the
    acquisition boundary. No physical-NAND parser is provided for this target.
    """
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


def _audio_xml(data: bytes, *, name: str) -> ET.Element:
    """Read one bounded original configuration without external resources."""
    try:
        # Original admission bounds these local bytes; ElementTree does not
        # resolve external entities or load configuration from other files.
        return ET.fromstring(data)  # noqa: S314
    except ET.ParseError as error:
        raise ValueError(f"invalid Qin audio configuration {name}") from error


def _speaker_mode(audio_hw: ET.Element) -> tuple[str, int]:
    """Resolve the supported normal speaker mode from the declared indices."""
    items = audio_hw.findall("./audiomode/item")
    modes = {item.get("name"): item.get("index") for item in items}
    if len(modes) != len(items) or modes != {
        "Headset": "0",
        "Headfree": "1",
        "Handset": "2",
        "Handsfree": "3",
    }:
        message = "unsupported Qin audio mode mapping"
        raise ValueError(message)
    return "Handsfree", 3


def _mainmic_route(tiny_hw: ET.Element) -> None:
    """Require the analog route and LR selections owned by the current driver."""
    paths = tiny_hw.findall("./device[@name='builtin-mic']/path[@name='on']")
    if len(paths) != 1:
        message = "missing Qin builtin microphone route"
        raise ValueError(message)
    controls = {item.get("name"): item.get("val") for item in paths[0].findall("ctl")}
    if controls != {
        "ADC LRCLK Select": "invert",
        "ADC1 LRCLK Select": "invert",
        "ADCL Mixer MainMICADCL Switch": "1",
        "Mic Function": "1",
    }:
        message = "unsupported Qin builtin microphone route"
        raise ValueError(message)


def _audio_parameter(data: bytes, key: str) -> int:
    """Read one required complete scalar without interpreting unrelated use cases."""
    text = data.decode("ascii")
    if "\0" in text:
        message = "NUL in Qin audio parameters"
        raise ValueError(message)
    values = [line.partition("=")[2] for line in text.splitlines() if line.startswith(key + "=")]
    if len(values) != 1:
        raise ValueError(f"missing or duplicate Qin audio parameter {key}")
    value = _parse_number(values[0].strip())
    if not 0 <= value <= 0xFFFFFFFF:
        message = "Qin audio parameter exceeds its unsigned word"
        raise ValueError(message)
    return value


def _packed_audio_control(codec: ET.Element, profile: str, name: str, word: int) -> int:
    """Apply the declared packed-field shift, width and inverse attribute."""
    fields = codec.findall(f"./profile[@name='{profile}']/pga[@name='{name}']")
    attributes = codec.findall(f"./attribute/mixer[@name='{name}']/attr")
    if len(fields) != 1 or len(attributes) != 1:
        raise ValueError(f"missing or duplicate Qin audio control {name}")
    try:
        shift = int(fields[0].attrib["bit"])
        maximum = int(attributes[0].attrib["max"])
        inverse = int(attributes[0].attrib["inverse"])
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid Qin audio control {name}") from error
    if not 0 <= shift < 32 or not 0 < maximum <= 127 or inverse not in (0, 1):
        raise ValueError(f"unsupported Qin audio control {name}")
    value = (word >> shift) & ((1 << maximum.bit_length()) - 1)
    if value > maximum:
        raise ValueError(f"Qin audio control exceeds its maximum: {name}")
    return maximum - value if inverse else value


def _signed_audio_word(value: int) -> int:
    """Admit the complete sign-extended coefficient representation."""
    signed: int = struct.unpack("<h", struct.pack("<H", value & 0xFFFF))[0]
    if value != signed & 0xFFFFFFFF:
        message = "Qin audio processing word is not a signed 16-bit value"
        raise ValueError(message)
    return signed


def _speaker_processing(data: bytes, *, mode: str, index: int) -> tuple[int, list[int]]:
    """Extract only the selected 44.1 kHz EQ6, output scale and ALC fields."""
    if len(data) < 24:
        message = "truncated Qin audio processing header"
        raise ValueError(message)
    magic, version, total, da_count, ad01_count, ad23_count = struct.unpack_from("<4s5I", data)
    required = 24 + 692 * da_count + 388 * (ad01_count + ad23_count)
    if (
        magic != b"VBEQ"
        or version != 2
        or total != da_count + ad01_count + ad23_count
        or not 0 < total <= 100
        or da_count <= index
        or len(data) < required
    ):
        message = "unsupported or truncated Qin audio processing profiles"
        raise ValueError(message)
    offset = 24 + index * 692
    profile_magic, name = struct.unpack_from("<4s32s", data, offset)
    if profile_magic != b"VBEQ" or name.split(b"\0", 1)[0] != mode.encode("ascii"):
        message = "Qin speaker processing profile does not match its mode"
        raise ValueError(message)
    words = struct.unpack_from("<164I", data, offset + 36)
    control = words[1]
    if control & ~0xB7F or control & 0xA00 != 0xA00:
        message = "unsupported Qin speaker processing blocks"
        raise ValueError(message)
    if any(words[position] for position in (0, 15, 16, 17, 18, 19)):
        message = "unsupported Qin speaker processing routing"
        raise ValueError(message)
    coefficients = []
    for coefficient in range(43):
        if words[21 + 2 * coefficient]:
            message = "Qin EQ6 coefficient requires an unsupported low word"
            raise ValueError(message)
        coefficients.append(_signed_audio_word(words[20 + 2 * coefficient]))
    output_scale = coefficients[42]
    if not 0 < output_scale <= 32767:
        message = "invalid Qin speaker output scale"
        raise ValueError(message)
    sections: list[int] = []
    for section in range(6):
        scale, b0, a0, b1, minus_a1, b2, minus_a2 = coefficients[7 * section : 7 * section + 7]
        if (
            a0 != 16384
            or scale <= 0
            or abs(minus_a2) >= 16384
            or abs(minus_a1) >= 16384 - minus_a2
        ):
            message = "unstable or unsupported Qin EQ6 section"
            raise ValueError(message)
        sections.extend((scale, b0, b1, minus_a1, b2, minus_a2))
    alc = [_signed_audio_word(word) for word in words[2:13]]
    return control & 0xFF, [output_scale, *alc, *sections]


def _prepare_audio_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Prepare normal speaker processing and MainMIC gain from admitted originals."""
    mode, index = _speaker_mode(_audio_xml(originals["audio_hw.xml"], name="audio_hw.xml"))
    _mainmic_route(_audio_xml(originals["tiny_hw.xml"], name="tiny_hw.xml"))
    codec = _audio_xml(originals["codec_pga.xml"], name="codec_pga.xml")
    prefix = f"audio_arm\\audio_arm\\{mode}\\AudioStructure\\"
    speaker = _audio_parameter(
        originals["audio_para"],
        prefix
        + "app_config_info_set\\app_config_info\\app_config_info[0]\\arm_volume\\arm_volume[1]",
    )
    microphone = _audio_parameter(originals["audio_para"], prefix + "reserve\\reserve[43]")
    dac = _packed_audio_control(codec, "speaker-l", "DAC Playback Volume", speaker)
    dg = _packed_audio_control(codec, "speaker-l", "VBC DACL DG Set", speaker)
    dg_enabled = _packed_audio_control(codec, "speaker-l", "VBC DACL DG Switch", speaker)
    right = (
        _packed_audio_control(codec, "speaker-r", "DAC Playback Volume", speaker),
        _packed_audio_control(codec, "speaker-r", "VBC DACR DG Set", speaker),
        _packed_audio_control(codec, "speaker-r", "VBC DACR DG Switch", speaker),
    )
    if right != (dac, dg, dg_enabled) or dac > 2 or dg > 127 or dg_enabled != 1:
        message = "unsupported Qin speaker gain controls"
        raise ValueError(message)
    mainmic = _packed_audio_control(codec, "capture-l", "ADCL Capture Volume", microphone)
    if mainmic > 7:
        message = "Qin MainMIC gain exceeds the analog field"
        raise ValueError(message)
    limit, processing = _speaker_processing(originals["vbc_eq"], mode=mode, index=index)
    payload = struct.pack(
        "<8sI7BH11h36h",
        _AUDIO_MAGIC,
        _AUDIO_RATE,
        2 - dac,
        dg,
        dg_enabled,
        mainmic,
        limit,
        1,
        1,
        *processing,
    )
    return DeviceDataPreparation(
        groups={
            "audio-profile": PreparedGroup(
                originals=dict(originals), prepared={"qin1splus-audio-profile.bin": payload}
            )
        }
    )


def prepare_from_originals(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
    """Prepare one complete admitted Qin radio or audio original set."""
    names = set(originals)
    if names == _ORIGINAL_NAMES:
        return _prepare_radio_from_originals(originals)
    if names == _AUDIO_ORIGINAL_NAMES:
        return _prepare_audio_from_originals(originals)
    message = "Qin preparation requires one complete audio or radio original set"
    raise ValueError(message)
