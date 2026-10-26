# SPDX-License-Identifier: GPL-2.0-only
"""Shared original-file reuse preserves independent groups and tracks actual producers."""

from __future__ import annotations

import json
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.cache import current_original_files, prepared_originals_current
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.original_files import save_original_files
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools, prepare_from_originals

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def original_spec() -> DeviceDataSpec:
    """Use a target transform double while exercising real admission and atomic publication."""
    return DeviceDataSpec(
        target="example",
        parser=None,
        originals_parser=ROOT / "tests/fixtures/original-files/audio_parser.py",
        preparation_recipe="b" * 64,
        audio=AudioProfileContext(
            machine_compatible=b"test,phone",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        groups={
            "audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}],
            "wcn-firmware": [{"source": "radio.bin", "destination": "radio.bin", "size": 5}],
        },
        required_groups=("audio-profile",),
        emmc={
            "raw_device": "/dev/mmcblk1",
            "acquisition_recipe": "a" * 64,
            "files": [
                {
                    "name": "audio.conf",
                    "locations": [["system", "/etc/audio.conf"]],
                    "max_bytes": 64,
                }
            ],
        },
    )


def test_audio_refresh_preserves_optional_radio_without_claiming_its_source(
    tmp_path: Path, original_spec: DeviceDataSpec
) -> None:
    phone = tmp_path / "phone"
    radio = phone / "groups/wcn-firmware"
    radio.mkdir(parents=True)
    (radio / "radio.bin").write_bytes(b"radio")
    manifest = save_original_files(
        "example",
        {"audio.conf": b"source settings"},
        tmp_path / "originals",
        acquisition_recipe="a" * 64,
    )
    source = current_original_files(original_spec, manifest.parent)
    assert source is not None

    prepare_from_originals(original_spec, manifest, phone, DeviceDataTools(), replace=True)

    assert (radio / "radio.bin").read_bytes() == b"radio"
    assert (phone / "groups/audio-profile/audio.bin").read_bytes() == b"encoded:source settings"
    receipt = json.loads((phone / "receipt.json").read_bytes())
    assert [group["name"] for group in receipt["groups"]] == ["audio-profile"]
    assert prepared_originals_current(original_spec, DeviceDataTools(), phone, source)

    changed = replace(original_spec, preparation_recipe="c" * 64)
    assert current_original_files(changed, manifest.parent) == source
    assert not prepared_originals_current(changed, DeviceDataTools(), phone, source)
    prepare_from_originals(changed, manifest, phone, DeviceDataTools(), replace=True)
    assert prepared_originals_current(changed, DeviceDataTools(), phone, source)
    assert (radio / "radio.bin").read_bytes() == b"radio"


def test_new_acquisition_does_not_reuse_files_from_another_reader(
    tmp_path: Path, original_spec: DeviceDataSpec
) -> None:
    manifest = save_original_files(
        "example",
        {"audio.conf": b"source settings"},
        tmp_path / "originals",
        acquisition_recipe="a" * 64,
    )
    assert current_original_files(original_spec, manifest.parent) is not None
    assert original_spec.emmc is not None
    changed = replace(original_spec, emmc={**original_spec.emmc, "acquisition_recipe": "c" * 64})

    assert current_original_files(changed, manifest.parent) is None
    assert (manifest.parent / "audio.conf").read_bytes() == b"source settings"


def test_invalid_new_source_keeps_saved_originals(tmp_path: Path) -> None:
    destination = tmp_path / "originals"
    save_original_files(
        "example", {"audio.conf": b"saved"}, destination, acquisition_recipe="a" * 64
    )

    with pytest.raises(SystemExit, match="nonempty"):
        save_original_files(
            "example", {"audio.conf": b""}, destination, acquisition_recipe="b" * 64
        )

    assert (destination / "audio.conf").read_bytes() == b"saved"
    assert (
        json.loads((destination / "manifest.json").read_bytes())["acquisition_recipe"] == "a" * 64
    )
