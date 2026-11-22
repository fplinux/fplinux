# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Prepare fitted audio and external Marlin2 firmware from the itel IT9310 NAND."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.common import sha256_bytes
from fplinux_cli.device_data.audio_profile import AUDIO_RECORD_SIZES, prepare_headset_gain_profile
from fplinux_cli.device_data.formats import (
    DeviceDataPreparation,
    RequiredPartition,
    fixed_nv_records,
)
from fplinux_cli.device_data.marlin2 import (
    RADIO_RECORD_SIZES,
    prepare_radio_calibration,
    prepare_radio_group,
)
from fplinux_cli.device_data.partitions import discover_partitions
from fplinux_cli.device_data.running_nv import read_running_nv_record

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import AudioProfileContext, PhysicalNand

_PREFIX = "itel-neo-r60-plus"
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
_WCN_IMAGE_BYTES = 592896
_WCN_IMAGE_SHA256 = "453f52e204aa1211c99ef81ec665340cffff917a0da7ea84f46a18f6d8b0261e"
_RUNNING_NV_VIRTUAL_BLOCKS = 40
_RADIO_IDENTITY_SIZES = {401: 8, 416: 6}


def prepare_device_data(
    nand: PhysicalNand, *, audio: AudioProfileContext
) -> DeviceDataPreparation:
    """Use fitted audio, calibration and current radio addresses from the same handset."""
    partitions = discover_partitions(nand, _REQUIRED_PARTITIONS).extents
    offset, size = partitions[_WCN_CODE]
    if size < _WCN_IMAGE_BYTES:
        message = "external radio partition is too small"
        raise ValueError(message)
    image = nand.partition_bytes((offset, _WCN_IMAGE_BYTES))
    if len(image) != _WCN_IMAGE_BYTES or sha256_bytes(image) != _WCN_IMAGE_SHA256:
        message = "external radio image does not match supported itel firmware"
        raise ValueError(message)
    required_sizes = AUDIO_RECORD_SIZES | RADIO_RECORD_SIZES | _RADIO_IDENTITY_SIZES
    downloaded = fixed_nv_records(nand.partition_bytes(partitions[_DOWNLOADED_NV]), required_sizes)
    protected = fixed_nv_records(nand.partition_bytes(partitions[_PROTECT_NV]), required_sizes)
    calibration = prepare_radio_calibration(downloaded, protected)
    identity = {}
    for identifier, record_size in _RADIO_IDENTITY_SIZES.items():
        current = read_running_nv_record(
            nand,
            partitions[_RUNNING_NV],
            identifier=identifier,
            size=record_size,
            virtual_blocks=_RUNNING_NV_VIRTUAL_BLOCKS,
        )
        if current != downloaded[identifier] or current != protected[identifier]:
            raise ValueError(f"RunningNV and fixed NV{identifier} disagree")
        identity[identifier] = current
    mac = identity[416]
    if mac == bytes(6) or mac[0] & 1:
        message = "RunningNV Wi-Fi address must be nonzero unicast"
        raise ValueError(message)
    profile = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix=_PREFIX,
        machine_compatible=audio.machine_compatible,
        speaker_vibration=audio.speaker_vibration,
        speaker_from_headphones=audio.speaker_from_headphones,
        headfree_headphones_only=audio.headfree_headphones_only,
    )
    radio = prepare_radio_group(
        image, calibration, downloaded, mac=mac, bluetooth_settings=identity[401], prefix=_PREFIX
    )
    return DeviceDataPreparation(groups={"audio-profile": profile, "wcn-firmware": radio})
