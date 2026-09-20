# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for one-source device-data generation publication."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from fplinux_cli import device_data_prepare
from fplinux_cli.device_data import DeviceDataPreparation, PhysicalNand, PreparedGroup

TARGET = "demo-phone"
RAW_PAGE_BYTES = 2112
RAW_DUMP = b"controlled physical NAND backup"
BLUETOOTH = PreparedGroup(
    originals={"radio-original.bin": b"fitted radio"},
    prepared={"radio.bin": b"radio"},
)
AUDIO_PROFILE = PreparedGroup(
    originals={"nv425.bin": b"mode", "nv426.bin": b"headset", "nv440.bin": b"eq"},
    prepared={"audio.bin": b"profile"},
)


class DeviceDataPrepareTests(unittest.TestCase):
    """Replace only phone access while exercising real staging and pointer publication."""

    def setUp(self) -> None:
        """Create an isolated source root and immutable saved dump."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.cache = self.root / ".cache"
        self.cache.mkdir(parents=True)
        self.saved_dump = self.root / "saved-nand.bin"
        self.saved_dump.write_bytes(RAW_DUMP)

    @staticmethod
    def _group(source: str, destination: str, size: int) -> list[dict[str, object]]:
        return [
            {
                "source": source,
                "destination": destination,
                "size": size,
            }
        ]

    def _config(
        self,
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
    ) -> dict[str, object]:
        groups: dict[str, list[dict[str, object]]] = {}
        if "bluetooth" in group_names:
            groups["bluetooth"] = self._group("radio.bin", "chip/radio.bin", 5)
        if "audio-profile" in group_names:
            groups["audio-profile"] = self._group(
                "audio.bin",
                "fplinux/audio.bin",
                7,
            )
        return {
            "device_data": {"parser": "device_data.py", "groups": groups},
            "nand": {"raw_page_bytes": RAW_PAGE_BYTES},
        }

    @staticmethod
    def _preparation(
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
        *,
        audio: PreparedGroup = AUDIO_PROFILE,
    ) -> DeviceDataPreparation:
        groups = {}
        if "bluetooth" in group_names:
            groups["bluetooth"] = BLUETOOTH
        if "audio-profile" in group_names:
            groups["audio-profile"] = audio
        return DeviceDataPreparation(groups=groups)

    def _run_from_dump(
        self,
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
        *,
        preparation: DeviceDataPreparation | None = None,
    ) -> str:
        parse = mock.Mock(return_value=preparation or self._preparation(group_names))
        parser = SimpleNamespace(prepare_device_data=parse)
        admitted = PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES)
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=self._config(group_names),
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_device_data_parser",
                return_value=parser,
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                return_value=admitted,
            ),
            mock.patch.object(
                device_data_prepare,
                "build",
                side_effect=AssertionError("saved-dump preparation must not build a loader"),
            ),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=AssertionError("saved-dump preparation must not load a phone"),
            ),
            mock.patch.object(
                device_data_prepare,
                "backup_target_nand",
                side_effect=AssertionError("saved-dump preparation must not access SSH"),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=3,
                offline=True,
            )
        return stdout.getvalue()

    def _current_generation(self) -> Path:
        target_root = self.cache / "device-data" / TARGET
        generation_name = (target_root / "current").read_text(encoding="ascii").strip()
        return target_root / "generations" / generation_name

    def test_saved_dump_publishes_both_groups_with_one_private_source_receipt(self) -> None:
        """One immutable input is copied once and owns provenance for both independent groups."""
        output = self._run_from_dump()

        generation = self._current_generation()
        self.assertEqual(self.saved_dump.read_bytes(), RAW_DUMP)
        self.assertEqual((generation / "source/nand.bin").read_bytes(), RAW_DUMP)
        self.assertEqual((generation / "groups/bluetooth/radio.bin").read_bytes(), b"radio")
        self.assertEqual(
            (generation / "groups/audio-profile/audio.bin").read_bytes(),
            b"profile",
        )
        self.assertEqual(
            (generation / "originals/audio-profile/nv426.bin").read_bytes(),
            b"headset",
        )
        receipt = json.loads((generation / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(
            receipt["source"],
            {
                "kind": "saved-dump",
                "size": len(RAW_DUMP),
                "sha256": hashlib.sha256(RAW_DUMP).hexdigest(),
            },
        )
        self.assertEqual(
            [group["name"] for group in receipt["groups"]],
            ["audio-profile", "bluetooth"],
        )
        self.assertNotIn(str(self.saved_dump), json.dumps(receipt))
        self.assertIn("./fplinux build demo-phone", output)

    def test_failed_staging_does_not_switch_the_current_generation(self) -> None:
        """A partial or malformed requested group cannot expose a mixed current state."""
        self._run_from_dump()
        target_root = self.cache / "device-data" / TARGET
        pointer = target_root / "current"
        previous_pointer = pointer.read_bytes()
        previous_generation = self._current_generation()
        malformed = PreparedGroup(
            originals=AUDIO_PROFILE.originals,
            prepared={"audio.bin": b"wrong size"},
        )

        with self.assertRaisesRegex(SystemExit, "device-data group audio-profile"):
            self._run_from_dump(preparation=self._preparation(audio=malformed))

        self.assertEqual(pointer.read_bytes(), previous_pointer)
        self.assertEqual(self._current_generation(), previous_generation)
        self.assertEqual(
            (previous_generation / "groups/audio-profile/audio.bin").read_bytes(),
            b"profile",
        )
        staging = list((target_root / "generations").glob(".staging-*"))
        self.assertEqual(staging, [])

    def test_live_acquisition_runs_once_and_the_same_raw_feeds_both_groups(self) -> None:
        """One public NAND backup is the observable safety boundary for all parser outputs."""
        events: list[object] = []
        admitted = PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES)

        def backup(target: str, destination: Path) -> Path:
            events.append(("backup", target))
            destination.write_bytes(RAW_DUMP)
            destination.chmod(0o600)
            return destination

        def extract(nand: PhysicalNand) -> DeviceDataPreparation:
            events.append(("extract", nand))
            return self._preparation()

        def admit(raw: bytes, *, page_bytes: int) -> PhysicalNand:
            events.append(("admit", raw, page_bytes))
            return admitted

        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=self._config(),
            ),
            mock.patch.object(
                device_data_prepare,
                "build",
                side_effect=lambda *_args, **_kwargs: events.append("build"),
            ),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=lambda *_args, **_kwargs: events.append("load"),
            ),
            mock.patch.object(device_data_prepare, "backup_target_nand", side_effect=backup),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                side_effect=admit,
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_device_data_parser",
                return_value=SimpleNamespace(prepare_device_data=extract),
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=None,
                jobs=2,
                offline=True,
            )

        self.assertEqual(
            events,
            [
                "build",
                "load",
                ("backup", TARGET),
                ("admit", RAW_DUMP, RAW_PAGE_BYTES),
                ("extract", admitted),
            ],
        )
        self.assertEqual((self._current_generation() / "source/nand.bin").read_bytes(), RAW_DUMP)

    def test_bluetooth_audio_and_combined_targets_publish_only_their_declared_groups(self) -> None:
        """Neither named group creates a dependency on the other."""
        for group_names in (("bluetooth",), ("audio-profile",), ("bluetooth", "audio-profile")):
            with self.subTest(groups=group_names):
                self._run_from_dump(group_names)
                published = self._current_generation() / "groups"
                self.assertEqual(
                    {path.name for path in published.iterdir()},
                    set(group_names),
                )

    def test_target_without_declared_groups_stops_before_source_or_phone_access(self) -> None:
        """Unsupported preparation cannot acquire a source or create a generation."""
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value={"device_data": {"groups": {}}},
            ),
            mock.patch.object(device_data_prepare, "build") as build,
            self.assertRaisesRegex(SystemExit, "not supported for target demo-phone"),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=None,
                jobs=1,
                offline=True,
            )
        build.assert_not_called()
        self.assertFalse((self.cache / "device-data").exists())


if __name__ == "__main__":
    unittest.main()
