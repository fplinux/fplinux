# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare the BQ 2830L playback profile and fitted external-radio inputs."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

from fplinux_cli.common import sha256_bytes
from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headset_gain_profile
from fplinux_cli.device_data.bq2830l_radio_nv import read_running_nv_record
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
    RequiredPartition,
    fixed_nv_records,
    nv_checksum,
    redundant_vbm_partitions,
)

if TYPE_CHECKING:
    from collections.abc import Mapping

_VBM_COPY_OFFSETS = (0x7FC0000, 0x7FE0000)
_DOWNLOADED_NV = 0x10000001
_RUNNING_NV = 0x10000003
_PROTECT_NV = 0x1000000F
_WCN_CODE = 0x10000014
_REQUIRED_PARTITIONS = {
    _DOWNLOADED_NV: RequiredPartition("DownloadedNV", 0x100),
    _RUNNING_NV: RequiredPartition("RunningNV", 0x001),
    _PROTECT_NV: RequiredPartition("ProtectNV", 0x100),
    _WCN_CODE: RequiredPartition("External radio code", 0x100),
}
_RADIO_RECORD_SIZES = {409: 888, 417: 3292}
_WCN_IMAGE_BYTES = 592896
_WCN_IMAGE_SHA256 = "5fdd81da4e8ef9a520dcb1d61543b0e62cb258be8b7a8966ffd648bccc3d5690"
_RADIO_CONFIG_BYTES = 616
_RADIO_IMAGE_NAME = "bq-2830l-tank-connect-max-wcn.bin"
_RADIO_CALIBRATION_NAME = "bq-2830l-tank-connect-max-wcn-calibration.bin"
_RADIO_MAC_NAME = "bq-2830l-tank-connect-max-wcn-mac.bin"
_RADIO_BLUETOOTH_NAME = "bq-2830l-tank-connect-max-wcn-bluetooth.bin"
_RUNNING_NV_HEADER = struct.Struct("<HHHHI")
_RUNNING_NV_SHORT_SLOT_BYTES = 32
_RUNNING_NV_VIRTUAL_BLOCKS = 40


def prepare_radio_mac(partition: bytes) -> bytes:
    """Require one saved nonzero unicast address among admitted short NV records.

    Physical positions do not establish which stored copy is active. All
    checksum-valid ordinary address records must agree.
    """
    addresses: set[bytes] = set()
    record_bytes = _RUNNING_NV_HEADER.size + 6
    for start in range(0, len(partition) - record_bytes + 1, _RUNNING_NV_SHORT_SLOT_BYTES):
        header_checksum, data_checksum, identifier, length, kind = _RUNNING_NV_HEADER.unpack_from(
            partition, start
        )
        if (identifier, length, kind) != (416, 6, 1):
            continue
        header = partition[start + 4 : start + _RUNNING_NV_HEADER.size]
        if header_checksum != nv_checksum(header):
            continue
        address = partition[start + _RUNNING_NV_HEADER.size : start + record_bytes]
        if data_checksum != nv_checksum(address) or address == bytes(6) or address[0] & 1:
            continue
        addresses.add(address)
    if not addresses:
        message = "no checksum-valid nonzero unicast RunningNV Wi-Fi address"
        raise ValueError(message)
    if len(addresses) != 1:
        message = "ambiguous RunningNV Wi-Fi addresses"
        raise ValueError(message)
    return next(iter(addresses))


def prepare_radio_calibration(
    downloaded: Mapping[int, bytes], protected: Mapping[int, bytes]
) -> bytes:
    """Retain matching fitted configuration and already calibrated radio records."""
    for identifier, size in _RADIO_RECORD_SIZES.items():
        record = downloaded.get(identifier)
        if record is None or len(record) != size:
            raise ValueError(f"radio NV{identifier} is missing or has the wrong size")
        if protected.get(identifier) != record:
            raise ValueError(f"radio NV{identifier} differs between fixed copies")
    configuration = downloaded[409]
    calibration = downloaded[417]
    if configuration[0] != 2 or calibration[0] != 2:
        message = "unsupported radio configuration or calibration version"
        raise ValueError(message)
    if calibration[1] != 1:
        message = "radio record is not already calibrated"
        raise ValueError(message)
    return configuration[:_RADIO_CONFIG_BYTES] + calibration


def prepare_bluetooth_pskey(configuration: bytes, settings: bytes) -> bytes:
    """Combine admitted radio configuration and the selected eight-byte BT record."""
    if len(configuration) != 888 or configuration[0] != 2:
        message = "unsupported external Bluetooth radio configuration"
        raise ValueError(message)
    if len(settings) != 8:
        message = "saved Bluetooth record has the wrong size"
        raise ValueError(message)
    if settings[:6] == bytes(6):
        message = "saved Bluetooth address is zero"
        raise ValueError(message)
    prepared = bytearray(220)
    prepared[0x00:0x3C] = configuration[0x26C:0x2A8]
    prepared[0x3C:0x4C] = configuration[0x310:0x320]
    prepared[0x4C:0x84] = configuration[0x320:0x358]
    prepared[0x4C:0x52] = settings[:6]
    prepared[0x84:0xA4] = configuration[0x2A8:0x2C8]
    prepared[0xA4:0xC4] = configuration[0x2C8:0x2E8]
    prepared[0xC4:0xC8] = configuration[0x2E8:0x2EC]
    for index in range(8):
        source = 0x2EC + index * 4
        destination = 0xC8 + index * 2
        prepared[destination : destination + 2] = configuration[source : source + 2]
    return bytes(prepared)


def _prepare_radio_firmware(
    code: bytes,
    running: bytes,
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    bluetooth_settings: bytes,
) -> PreparedGroup:
    image = code[:_WCN_IMAGE_BYTES]
    if len(image) != _WCN_IMAGE_BYTES or sha256_bytes(image) != _WCN_IMAGE_SHA256:
        message = "external radio image does not match the supported firmware"
        raise ValueError(message)
    calibration = prepare_radio_calibration(downloaded, protected)
    mac = prepare_radio_mac(running)
    pskey = prepare_bluetooth_pskey(downloaded[409], bluetooth_settings)
    return PreparedGroup(
        originals={
            _RADIO_IMAGE_NAME: image,
            "nv409.bin": downloaded[409],
            "nv417.bin": downloaded[417],
            "nv416.bin": mac,
            "nv401.bin": bluetooth_settings,
        },
        prepared={
            _RADIO_IMAGE_NAME: image,
            _RADIO_CALIBRATION_NAME: calibration,
            _RADIO_MAC_NAME: mac,
            _RADIO_BLUETOOTH_NAME: pskey,
        },
    )


def prepare_device_data(nand: PhysicalNand) -> DeviceDataPreparation:
    """Retain matching fixed data, saved Wi-Fi identity and current Bluetooth settings."""
    partitions = redundant_vbm_partitions(nand, _VBM_COPY_OFFSETS, _REQUIRED_PARTITIONS)
    required_sizes = {**AUDIO_RECORD_SIZES, **_RADIO_RECORD_SIZES}
    downloaded = fixed_nv_records(nand.partition_bytes(partitions[_DOWNLOADED_NV]), required_sizes)
    protected = fixed_nv_records(nand.partition_bytes(partitions[_PROTECT_NV]), required_sizes)
    bluetooth_settings = read_running_nv_record(
        nand,
        partitions[_RUNNING_NV],
        identifier=401,
        size=8,
        virtual_blocks=_RUNNING_NV_VIRTUAL_BLOCKS,
    )
    profile = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix="bq-2830l-tank-connect-max",
        machine_compatible=b"bq,2830l",
        speaker_from_headphones=True,
    )
    radio = _prepare_radio_firmware(
        nand.partition_bytes(partitions[_WCN_CODE]),
        nand.partition_bytes(partitions[_RUNNING_NV]),
        downloaded,
        protected,
        bluetooth_settings=bluetooth_settings,
    )
    return DeviceDataPreparation(groups={"audio-profile": profile, "wcn-firmware": radio})
