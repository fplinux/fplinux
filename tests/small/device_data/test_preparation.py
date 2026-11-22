# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data preparation scenarios."""

from __future__ import annotations

import hashlib
import importlib.util

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.device_data import (
    fitted as fitted_device_data,
)
from fplinux_cli.device_data import (
    formats as device_data,
)
from fplinux_cli.manifests.targets import load_target

from tests.small.device_data.audio_expected import (
    FITTED_HEADFREE_SECTION,
    FITTED_INOI_PROCESSING_SECTION,
    INOI_VIBRATE_TONE_SECTION,
)
from tests.small.device_data.audio_fixtures import inoi_audio_records
from tests.small.device_data.cm4_fixtures import cm4_firmware
from tests.small.device_data.nv_fixtures import fixed_records, nv1, radio_records, running_nv
from tests.small.device_data.partition_fixtures import vbm_table


class PartitionPreparationTests:
    """Exercise extraction and copy preservation on synthetic physical pages."""

    @staticmethod
    def _inputs(
        protected_address: bytes = b"A" * 8,
        protected_fm: bytes = bytes(range(128)),
        *,
        oob_bytes: int = 64,
        external_radio: bool = False,
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
        if external_radio:
            downloaded_records.update(radio_records())
            protected_records.update(radio_records())
            downloaded_records = dict(sorted(downloaded_records.items()))
            protected_records = dict(sorted(protected_records.items()))
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
        ("target", "prefix", "oob_bytes"),
        [
            ("nokia-ta1618", "ta1618", 64),
            ("nokia-110-4g", "nokia110", 64),
            ("inoi-240-modern-4g", "inoi240", 64),
            ("inoi-244-modern-4g", "inoi244", 64),
            ("inoi-244-modern-4g", "inoi244", 128),
            ("maxvi-k15n-4g", "maxvi-k15n-4g", 64),
        ],
    )
    def test_fitted_preparation_uses_the_declared_prefix_and_discovers_partitions(
        self, target: str, prefix: str, oob_bytes: int
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
        fitted_prefix = load_target(target)["device_data"]["fitted_prefix"]
        audio = device_data.AudioProfileContext(
            machine_compatible=b"example,phone",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        )

        result = fitted_device_data.prepare_fitted_data(nand, audio=audio, prefix=fitted_prefix)

        profile = result.groups["audio-profile"].prepared[f"{prefix}-audio-profile.bin"]
        assert profile[:32] == b"FPAUDIO\0example,phone" + b"\0" * 11
        assert profile[53:] == FITTED_HEADFREE_SECTION + FITTED_INOI_PROCESSING_SECTION
        original = cm4_firmware()
        expected_cm4 = (
            original[:0x114] + b"\x08\xe0" + original[0x116:0x130] + b"\x2b\xe0" + original[0x132:]
        )
        assert result.groups["bluetooth"].originals[f"{prefix}-cm4.bin"] == original
        assert result.groups["bluetooth"].prepared[f"{prefix}-cm4.bin"] == expected_cm4

    @pytest.mark.parametrize("image_matches", [True, False], ids=["supported", "other-image"])
    def test_itel_prepares_external_radio_from_the_same_nand(
        self, monkeypatch: pytest.MonkeyPatch, *, image_matches: bool
    ) -> None:
        """The external image needs its own digest and matching fitted calibration."""
        target = "itel-neo-r60-plus"
        parser = load_target(target)["device_data"]["parser"]
        spec = importlib.util.spec_from_file_location(
            "test_itel_device_data", ROOT / "targets" / target / parser
        )
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        image = b"itel radio image" * 37056
        assert len(image) == 592896
        monkeypatch.setattr(module, "_WCN_IMAGE_SHA256", hashlib.sha256(image).hexdigest())
        stored_image = image if image_matches else b"?" + image[1:]
        nand, _partitions = self._inputs(oob_bytes=128, external_radio=True)
        table = vbm_table(
            (
                (0x10000018, 0x100, 0, 0),
                (0x10000001, 0x100, 1, 1),
                (0x1000000F, 0x100, 2, 2),
                (0x10000003, 1, 3, 3),
                (0x10000014, 0x100, 4, 8),
            )
        ).ljust(131072, b"\xff")
        main = stored_image.ljust(5 * 131072, b"\xff") + table + table
        raw = b"".join(
            main[offset : offset + 2048] + b"\xff" * 128 for offset in range(0, len(main), 2048)
        )
        nand = device_data.PhysicalNand(nand.raw + raw, 2176)
        audio = device_data.AudioProfileContext(
            machine_compatible=b"itel,neo-r60",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        )
        if not image_matches:
            with pytest.raises(ValueError, match="does not match supported itel firmware"):
                module.prepare_device_data(nand, audio=audio)
            return

        prepared = module.prepare_device_data(nand, audio=audio).groups

        assert set(prepared) == {"bluetooth", "audio-profile", "fm-radio", "wcn-firmware"}
        assert prepared["wcn-firmware"].prepared == {
            "itel-neo-r60-plus-wcn.bin": image,
            "itel-neo-r60-plus-wcn-calibration.bin": b"\x02"
            + b"C" * 615
            + b"\x02\x01"
            + b"K" * 3290,
        }

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
