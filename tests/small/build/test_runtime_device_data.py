# SPDX-License-Identifier: GPL-2.0-only
"""Verify the build-time selection of runtime phone-data descriptors."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.runtime import device_data_descriptor

if TYPE_CHECKING:
    from typing import Any


@pytest.fixture
def ram_phone() -> dict[str, Any]:
    """Supply a RAM target with a NAND source and an audio group to prepare."""
    return {
        "platform": "ums9117",
        "linux": {"root": {"kind": "initramfs"}},
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": True,
            "headfree_headphones_only": False,
        },
        "nand": {"raw_device": "/dev/raw-nand", "id": 0xB1A1, "raw_page_bytes": 2176},
        "device_data": {
            "parser": "example_device_data.py",
            "groups": {
                "audio-profile": [
                    {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 788}
                ]
            },
        },
    }


def test_ram_phone_descriptor_records_required_data_and_audio_policy(
    ram_phone: dict[str, Any],
) -> None:
    assert device_data_descriptor("example-phone", ram_phone) == {
        "parser": "targets/example-phone/example_device_data.py",
        "originals_parser": None,
        "groups": {
            "audio-profile": [
                {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 788}
            ]
        },
        "required_groups": ["audio-profile"],
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": True,
            "headfree_headphones_only": False,
        },
        "ecc_module": None,
    }


@pytest.mark.parametrize(
    ("section", "replacement"),
    [
        pytest.param("linux", {"root": {"kind": "external"}}, id="external-root"),
        pytest.param(
            "device_data",
            {"parser": "example_device_data.py", "groups": {}},
            id="no-data-groups",
        ),
        pytest.param("nand", None, id="no-data-source"),
    ],
)
def test_phone_missing_a_preparation_prerequisite_has_no_descriptor(
    ram_phone: dict[str, Any], section: str, replacement: dict[str, object] | None
) -> None:
    assert device_data_descriptor("example-phone", {**ram_phone, section: replacement}) is None
