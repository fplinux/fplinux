# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data preparation scenarios."""

from __future__ import annotations

import importlib.util

import pytest
from fplinux_cli.device_data import (
    fitted as fitted_device_data,
)
from fplinux_cli.device_data import (
    formats as device_data,
)

from tests import ROOT
from tests.small.device_data.audio_expected import (
    FITTED_HEADFREE_SECTION,
    FITTED_INOI_PROCESSING_SECTION,
    INOI_VIBRATE_TONE_SECTION,
)
from tests.small.device_data.audio_fixtures import inoi_audio_records
from tests.small.device_data.cm4_fixtures import cm4_firmware
from tests.small.device_data.nv_fixtures import fixed_records, nv1, running_nv
from tests.small.device_data.partition_fixtures import vbm_table


class PartitionPreparationTests:
    """Exercise extraction and copy preservation on synthetic physical pages."""

    @staticmethod
    def _inputs(
        protected_address: bytes = b"A" * 8,
        protected_fm: bytes = bytes(range(128)),
        *,
        oob_bytes: int = 64,
    ) -> tuple[
        device_data.PhysicalNand,
        dict[int, tuple[int, int]],
    ]:
        original = cm4_firmware()
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
            main[offset : offset + 2048] + b"\xff" * oob_bytes
            for offset in range(0, len(main), 2048)
        )
        partitions = {
            0x10000018: (0, 131072),
            0x10000001: (131072, 131072),
            0x1000000F: (262144, 131072),
            0x10000003: (393216, 131072),
        }
        return device_data.PhysicalNand(raw, 2048 + oob_bytes), partitions

    @pytest.mark.parametrize(
        ("target", "filename", "prefix", "oob_bytes"),
        [
            ("nokia-ta1618", "ta1618_device_data.py", "ta1618", 64),
            ("inoi-240-modern-4g", "inoi240_device_data.py", "inoi240", 64),
            ("inoi-244-modern-4g", "inoi244_device_data.py", "inoi244", 64),
            ("inoi-244-modern-4g", "inoi244_device_data.py", "inoi244", 128),
            ("maxvi-k15n-4g", "maxvi_device_data.py", "maxvi-k15n-4g", 64),
        ],
    )
    def test_target_parser_uses_the_selected_identity_and_speaker_policy(
        self, target: str, filename: str, prefix: str, oob_bytes: int
    ) -> None:
        """Target partition discovery retains the selected profile identity and topology."""
        nand, _partitions = self._inputs(oob_bytes=oob_bytes)
        table = vbm_table(
            (
                (0x10000018, 0x100, 0, 0),
                (0x10000001, 0x100, 1, 1),
                (0x1000000F, 0x100, 2, 2),
                (0x10000003, 1, 3, 3),
            )
        ).ljust(131072, b"\xff")
        table_raw = b"".join(
            table[offset : offset + 2048] + b"\xff" * oob_bytes
            for offset in range(0, len(table), 2048)
        )
        nand = device_data.PhysicalNand(nand.raw + table_raw * 2, 2048 + oob_bytes)
        spec = importlib.util.spec_from_file_location(
            f"test_{target.replace('-', '_')}_data", ROOT / "targets" / target / filename
        )
        assert spec is not None
        assert spec.loader is not None
        parser = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(parser)
        audio = device_data.AudioProfileContext(
            machine_compatible=b"example,phone",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        )

        result = parser.prepare_device_data(nand, audio=audio)

        profile = result.groups["audio-profile"].prepared[f"{prefix}-audio-profile.bin"]
        assert profile[:32] == b"FPAUDIO\0example,phone" + b"\0" * 11
        assert profile[53:] == FITTED_HEADFREE_SECTION + FITTED_INOI_PROCESSING_SECTION
        original = cm4_firmware()
        expected_cm4 = (
            original[:0x114] + b"\x08\xe0" + original[0x116:0x130] + b"\x2b\xe0" + original[0x132:]
        )
        assert result.groups["bluetooth"].originals[f"{prefix}-cm4.bin"] == original
        assert result.groups["bluetooth"].prepared[f"{prefix}-cm4.bin"] == expected_cm4

    def test_shared_fitted_extraction_includes_fm_and_rejects_conflicting_copies(self) -> None:
        """Every fitted target needs the same admitted FM group from matching NV419 copies."""
        nand, partitions = self._inputs()

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="phone",
            audio=device_data.AudioProfileContext(
                machine_compatible=b"vendor,phone",
                speaker_vibration=False,
                speaker_from_headphones=False,
                headfree_headphones_only=False,
            ),
        )

        assert (set(result.groups)) == ({"bluetooth", "audio-profile", "fm-radio"})
        assert (result.groups["fm-radio"].originals) == ({"phone-nv419.bin": bytes(range(128))})
        assert (result.groups["fm-radio"].prepared["phone-fm-config.bin"][:18]) == (
            bytes.fromhex("00 00 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 0e 0f")
        )

        conflicting = bytes(range(127)) + b"\0"
        nand, partitions = self._inputs(protected_fm=conflicting)
        with pytest.raises(ValueError, match="NV419 differs"):
            fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="phone",
                audio=device_data.AudioProfileContext(
                    machine_compatible=b"vendor,phone",
                    speaker_vibration=False,
                    speaker_from_headphones=False,
                    headfree_headphones_only=False,
                ),
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
        nand, partitions = self._inputs()

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="phone",
            audio=device_data.AudioProfileContext(
                machine_compatible=b"vendor,phone",
                speaker_vibration=speaker_vibration,
                speaker_from_headphones=False,
                headfree_headphones_only=False,
            ),
        )

        profile = result.groups["audio-profile"].prepared["phone-audio-profile.bin"]
        assert profile[53:] == (
            FITTED_HEADFREE_SECTION + FITTED_INOI_PROCESSING_SECTION + expected_tail
        )

    def test_complete_set_keeps_original_image_and_individual_nv_bytes(self) -> None:
        """Only the prepared CM4 changes; every original remains byte-exact."""
        nand, partitions = self._inputs()
        original = cm4_firmware()
        expected_cm4 = (
            original[:0x114] + b"\x08\xe0" + original[0x116:0x130] + b"\x2b\xe0" + original[0x132:]
        )
        expected_originals = {
            "example-cm4.bin": original,
            "example-bt-config.bin": b"A" * 8,
            "example-bt-sprd.bin": b"B" * 176,
            "example-bt-rf-config.bin": b"C" * 252,
        }

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="example",
            audio=device_data.AudioProfileContext(
                machine_compatible=b"vendor,phone",
                speaker_vibration=False,
                speaker_from_headphones=False,
                headfree_headphones_only=False,
            ),
        ).groups["bluetooth"]

        assert (result.originals) == (expected_originals)
        assert (result.prepared) == (expected_originals | {"example-cm4.bin": expected_cm4})

    def test_conflicting_protected_nv_cannot_produce_a_firmware_set(self) -> None:
        """Disagreeing fixed copies are rejected instead of selecting one address."""
        nand, partitions = self._inputs(protected_address=b"D" * 8)

        with pytest.raises(ValueError, match="DownloadedNV and ProtectNV disagree"):
            fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="example",
                audio=device_data.AudioProfileContext(
                    machine_compatible=b"vendor,phone",
                    speaker_vibration=False,
                    speaker_from_headphones=False,
                    headfree_headphones_only=False,
                ),
            )
