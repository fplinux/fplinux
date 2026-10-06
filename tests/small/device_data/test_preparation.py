# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data preparation scenarios."""

from __future__ import annotations

import hashlib

import pytest
from fplinux_cli.device_data import (
    bluetooth_firmware as bluetooth,
)
from fplinux_cli.device_data import (
    fitted as fitted_device_data,
)
from fplinux_cli.device_data import (
    formats as device_data,
)

from tests.small.device_data.audio_expected import (
    FITTED_HEADFREE_SECTION,
    FITTED_INOI_PROCESSING_SECTION,
    INOI_VIBRATE_TONE_SECTION,
)
from tests.small.device_data.audio_fixtures import inoi_audio_records
from tests.small.device_data.nv_fixtures import fixed_records, nv1, running_nv


class PartitionPreparationTests:
    """Exercise extraction and copy preservation on synthetic physical pages."""

    @staticmethod
    def _inputs(
        protected_address: bytes = b"A" * 8,
        protected_fm: bytes = bytes(range(128)),
    ) -> tuple[
        device_data.PhysicalNand,
        dict[int, tuple[int, int]],
        bluetooth.Cm4Revision,
    ]:
        original = b"prefix\x2d\x4cmiddle\xe0\x6dsuffix"
        prepared = b"prefix\x08\xe0middle\x2b\xe0suffix"
        downloaded_audio, protected_audio = inoi_audio_records()
        downloaded_records = fixed_records() | {419: bytes(range(128))} | downloaded_audio
        protected_records = {
            401: protected_address,
            402: b"B" * 176,
            404: b"C" * 252,
            419: protected_fm,
        } | protected_audio
        payloads = (
            original,
            nv1(tuple(downloaded_records.items())),
            nv1(tuple(protected_records.items())),
            running_nv(),
        )
        main = b"".join(payload.ljust(131072, b"\xff") for payload in payloads)
        raw = b"".join(
            main[offset : offset + 2048] + b"\xff" * 64 for offset in range(0, len(main), 2048)
        )
        partitions = {
            0x10000018: (0, 131072),
            0x10000001: (131072, 131072),
            0x1000000F: (262144, 131072),
            0x10000003: (393216, 131072),
        }
        revision = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(prepared).hexdigest(),
            pub_policy_offsets=(6, 14),
        )
        return device_data.PhysicalNand(raw, 2112), partitions, revision

    def test_shared_fitted_extraction_includes_fm_and_rejects_conflicting_copies(self) -> None:
        """Every fitted target needs the same admitted FM group from matching NV419 copies."""
        nand, partitions, revision = self._inputs()

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="phone",
            revision=revision,
            machine_compatible=b"vendor,phone",
        )

        assert (set(result.groups)) == ({"bluetooth", "audio-profile", "fm-radio"})
        assert (result.groups["fm-radio"].originals) == ({"phone-nv419.bin": bytes(range(128))})
        assert (result.groups["fm-radio"].prepared["phone-fm-config.bin"][:18]) == (
            bytes.fromhex("00 00 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 0e 0f")
        )

        conflicting = bytes(range(127)) + b"\0"
        nand, partitions, revision = self._inputs(protected_fm=conflicting)
        with pytest.raises(ValueError, match="NV419 differs"):
            fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="phone",
                revision=revision,
                machine_compatible=b"vendor,phone",
            )

    @pytest.mark.parametrize(
        ("speaker_vibration", "expected_tail"),
        [
            pytest.param(False, b"", id="without-vibration"),
            pytest.param(True, INOI_VIBRATE_TONE_SECTION, id="with-vibration"),
        ],
    )
    def test_speaker_vibration_target_receives_the_vibrate_tone_section(
        self, *, speaker_vibration: bool, expected_tail: bytes
    ) -> None:
        """Partition preparation forwards speaker_vibration; only True appends the tone section."""
        nand, partitions, revision = self._inputs()

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="phone",
            revision=revision,
            machine_compatible=b"vendor,phone",
            speaker_vibration=speaker_vibration,
        )

        profile = result.groups["audio-profile"].prepared["phone-audio-profile.bin"]
        assert profile[53:] == (
            FITTED_HEADFREE_SECTION + FITTED_INOI_PROCESSING_SECTION + expected_tail
        )

    def test_complete_set_keeps_original_image_and_individual_nv_bytes(self) -> None:
        """Only the prepared CM4 changes; every original remains byte-exact."""
        nand, partitions, revision = self._inputs()
        expected_originals = {
            "example-cm4.bin": b"prefix\x2d\x4cmiddle\xe0\x6dsuffix",
            "example-bt-config.bin": b"A" * 8,
            "example-bt-sprd.bin": b"B" * 176,
            "example-bt-rf-config.bin": b"C" * 252,
        }

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="example",
            revision=revision,
            machine_compatible=b"vendor,phone",
        ).groups["bluetooth"]

        assert (result.originals) == (expected_originals)
        assert (result.prepared) == (
            expected_originals | {"example-cm4.bin": b"prefix\x08\xe0middle\x2b\xe0suffix"}
        )

    def test_conflicting_protected_nv_cannot_produce_a_firmware_set(self) -> None:
        """Disagreeing fixed copies are rejected instead of selecting one address."""
        nand, partitions, revision = self._inputs(protected_address=b"D" * 8)

        with pytest.raises(ValueError, match="DownloadedNV and ProtectNV disagree"):
            fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="example",
                revision=revision,
                machine_compatible=b"vendor,phone",
            )
