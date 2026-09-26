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

from fplinux_cli import common, device_data_prepare, workspace
from fplinux_cli.device_data import (
    DeviceDataPreparation,
    NandGeometry,
    PhysicalNand,
    PreparedGroup,
)
from fplinux_cli.environment import images, kern
from fplinux_cli.manifests import releases
from fplinux_cli.output import run_entrypoint

TARGET = "demo-phone"
RAW_PAGE_BYTES = 2112
DECLARED_NAND = {"raw_device": "/dev/demo-nand-raw", "id": 0x21E5, "raw_page_bytes": 2112}
RAW_DUMP = b"controlled physical NAND backup"
BLUETOOTH = PreparedGroup(
    originals={"radio-original.bin": b"fitted radio"},
    prepared={"radio.bin": b"radio"},
)
AUDIO_PROFILE = PreparedGroup(
    originals={"nv425.bin": b"mode", "nv426.bin": b"headset", "nv440.bin": b"eq"},
    prepared={"audio.bin": b"profile"},
)
BOARD_REPORT = b'{"keypad": {"rows": 5}}\n'
BOARD_MAPS = PreparedGroup(
    originals={"stock-image.bin": b"stock image"},
    prepared={"pinmap.bin": b"pins", "keymap.bin": b"keys"},
    reports={"board-report.json": BOARD_REPORT},
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
        self.enterContext(mock.patch("fplinux_cli.output.ROOT", self.root))
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
        *,
        nand: dict[str, object] = DECLARED_NAND,
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
            "nand": nand,
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

        def backup(target: str, destination: Path, **_logging: object) -> Path:
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

    def _run_board_maps_from_dump(
        self,
        *,
        with_bluetooth: bool,
        extract: object,
    ) -> str:
        """Prepare board maps with a stub platform extraction and a stub current build."""
        groups: dict[str, list[dict[str, object]]] = {
            "board-maps": [
                # Board maps are declared without sizes; each phone's own maps are admitted.
                {"source": "pinmap.bin", "destination": "pinmap.bin"},
                {"source": "keymap.bin", "destination": "keymap.bin"},
            ]
        }
        device_data: dict[str, object] = {"groups": groups}
        if with_bluetooth:
            groups["bluetooth"] = self._group("radio.bin", "chip/radio.bin", 5)
            device_data["parser"] = "device_data.py"
        parser = SimpleNamespace(
            prepare_device_data=lambda _nand: DeviceDataPreparation(
                groups={"bluetooth": BLUETOOTH}
            )
        )
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value={
                    "platform": "demo-platform",
                    "device_data": device_data,
                    "nand": DECLARED_NAND,
                },
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_device_data_parser",
                return_value=parser,
                side_effect=None if with_bluetooth else AssertionError("no parser group"),
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_board_maps_provider",
                return_value=SimpleNamespace(prepare_board_maps=extract),
            ),
            mock.patch.object(
                device_data_prepare,
                "resolve_target_bundle",
                return_value=(SimpleNamespace(path=self.root / "current-bundle"), {}),
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                return_value=PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=1,
                offline=True,
            )
        return stdout.getvalue()

    def test_board_maps_come_from_the_platform_with_current_build_tools_and_a_report(
        self,
    ) -> None:
        """Platform output is published beside parser groups; its report is not a build input."""
        received: list[Path] = []

        def extract(nand: PhysicalNand, *, host_tools: Path) -> PreparedGroup:
            self.assertEqual(nand.raw, RAW_DUMP)
            received.append(host_tools)
            return BOARD_MAPS

        output = self._run_board_maps_from_dump(with_bluetooth=True, extract=extract)

        generation = self._current_generation()
        self.assertEqual(received, [self.root / "current-bundle/host"])
        self.assertEqual((generation / "groups/board-maps/pinmap.bin").read_bytes(), b"pins")
        self.assertEqual((generation / "groups/board-maps/keymap.bin").read_bytes(), b"keys")
        self.assertEqual((generation / "groups/bluetooth/radio.bin").read_bytes(), b"radio")
        self.assertEqual(
            (generation / "originals/board-maps/stock-image.bin").read_bytes(), b"stock image"
        )
        report = generation / "reports/board-maps/board-report.json"
        self.assertEqual(report.read_bytes(), BOARD_REPORT)
        self.assertEqual(report.stat().st_mode & 0o777, 0o600)
        self.assertFalse((generation / "groups/board-maps/board-report.json").exists())
        receipt = json.loads((generation / "receipt.json").read_text(encoding="utf-8"))
        groups = {group["name"]: group for group in receipt["groups"]}
        self.assertEqual(
            groups["board-maps"]["reports"],
            [
                {
                    "name": "board-report.json",
                    "size": len(BOARD_REPORT),
                    "sha256": hashlib.sha256(BOARD_REPORT).hexdigest(),
                }
            ],
        )
        self.assertNotIn("reports", groups["bluetooth"])
        self.assertIn(f"Review {report}.", output)

    def test_board_maps_without_a_current_build_or_extraction_publish_nothing(self) -> None:
        """A missing host-tool build or a failed extraction leaves no generation."""

        def unavailable(_nand: PhysicalNand, *, host_tools: Path) -> PreparedGroup:
            del host_tools
            message = "board maps not found in the stock image"
            raise ValueError(message)

        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value={
                    "platform": "demo-platform",
                    "device_data": {"groups": {"board-maps": []}},
                    "nand": DECLARED_NAND,
                },
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_board_maps_provider",
                return_value=SimpleNamespace(prepare_board_maps=unavailable),
            ),
            self.assertRaisesRegex(
                SystemExit,
                "current build is missing or invalid; rebuild it: ./fplinux build demo-phone",
            ),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=1,
                offline=True,
            )
        self.assertFalse((self.cache / "device-data").exists())

        with self.assertRaisesRegex(
            SystemExit, "device-data extraction failed: board maps not found in the stock image"
        ):
            self._run_board_maps_from_dump(with_bluetooth=False, extract=unavailable)
        target_root = self.cache / "device-data" / TARGET
        self.assertFalse((target_root / "current").exists())
        self.assertEqual(list((target_root / "generations").iterdir()), [])

    def test_saved_dump_without_declared_chip_or_receipt_publishes_nothing(self) -> None:
        """A backup's page layout is never inferred from its length."""
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=self._config(nand={"raw_device": "/dev/demo-nand-raw"}),
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                side_effect=AssertionError("an unknown layout must not be admitted"),
            ),
            mock.patch.object(
                device_data_prepare,
                "_load_device_data_parser",
                side_effect=AssertionError("an unknown layout must not reach a parser"),
            ),
            self.assertRaisesRegex(SystemExit, "declares no NAND chip and the backup has no"),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=1,
                offline=True,
            )

        target_root = self.cache / "device-data" / TARGET
        self.assertFalse((target_root / "current").exists())
        self.assertEqual(list((target_root / "generations").iterdir()), [])
        self.assertEqual(self.saved_dump.read_bytes(), RAW_DUMP)

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

    def test_offline_nested_build_failure_leaves_no_running_receipt(self) -> None:
        """A real build rejection finishes its enclosing preparation without loading a phone."""
        terminal = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(device_data_prepare, "load_target", return_value=self._config()),
            mock.patch.object(
                releases, "load_release", return_value={"image": "image/ramboot.bin"}
            ),
            mock.patch.object(
                workspace,
                "target_workspace_snapshot",
                return_value=workspace.WorkspaceSnapshot((), "a" * 64),
            ),
            mock.patch.object(
                images,
                "load_container_lock",
                return_value={"oci": {"repository": "localhost/fplinux-build"}},
            ),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern, "kern_available", return_value=False),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=AssertionError("failed preparation must not load a phone"),
            ),
            contextlib.redirect_stderr(terminal),
            self.assertRaises(SystemExit) as raised,
        ):
            run_entrypoint(
                lambda: device_data_prepare.prepare_device_data(
                    TARGET, from_dump=None, jobs=1, offline=True
                )
            )
        self.assertEqual(raised.exception.code, 1)
        self.assertEqual(terminal.getvalue().count("offline build requires"), 1)
        receipts = list((self.cache / "logs").rglob("run.json"))
        self.assertTrue(receipts)
        for receipt in receipts:
            self.assertEqual(json.loads(receipt.read_text())["status"], "failed")


def _reported(  # noqa: PLR0913 -- each reported value stays visible at the call site.
    *,
    id_bytes: str,
    page_main_bytes: int = 2048,
    oob_bytes: int,
    pages_per_block: int = 64,
    block_count: int = 1024,
    raw_bytes: int,
) -> NandGeometry:
    return NandGeometry(
        id_bytes=id_bytes,
        chip="demo",
        page_main_bytes=page_main_bytes,
        oob_bytes=oob_bytes,
        pages_per_block=pages_per_block,
        block_count=block_count,
        raw_bytes=raw_bytes,
    )


class DumpGeometryTests(unittest.TestCase):
    """Select a saved backup's page layout without guessing it from the backup length."""

    def test_page_size_comes_from_the_receipt_or_the_declared_chip(self) -> None:
        """Either source is sufficient, and agreeing sources give the same page size."""
        receipt_2112 = _reported(id_bytes="e521", oob_bytes=64, raw_bytes=138412032)
        cases = (
            ("declared chip only", DECLARED_NAND, None, 2112),
            (
                "declared 128-byte OOB chip only",
                {"raw_device": "/dev/demo-nand-raw", "id": 0xB1A1, "raw_page_bytes": 2176},
                None,
                2176,
            ),
            ("receipt only", {"raw_device": "/dev/demo-nand-raw"}, receipt_2112, 2112),
            ("receipt without a NAND table", None, receipt_2112, 2112),
            ("receipt and declared chip agree", DECLARED_NAND, receipt_2112, 2112),
        )
        for name, nand, receipt, expected in cases:
            with self.subTest(name):
                self.assertEqual(
                    device_data_prepare.dump_page_bytes(TARGET, nand, receipt),
                    expected,
                )

    def test_missing_contradicting_or_unsupported_geometry_is_refused(self) -> None:
        """A receipt from another chip or outside the interpreted layout cannot be used."""
        cases = (
            ("neither source", {"raw_device": "/dev/demo-nand-raw"}, None, "declares no NAND"),
            (
                "receipt from another chip",
                DECLARED_NAND,
                _reported(id_bytes="a1b1", oob_bytes=128, raw_bytes=142606336),
                "reported id_bytes=a1b1 with 2176-byte pages; target declares id_bytes=e521",
            ),
            (
                "4 KiB main pages",
                None,
                _reported(
                    id_bytes="c8b4",
                    page_main_bytes=4096,
                    oob_bytes=256,
                    raw_bytes=285212672,
                ),
                "4096-byte main pages",
            ),
            (
                "half the pages",
                None,
                _reported(id_bytes="c8f1", oob_bytes=64, block_count=512, raw_bytes=69206016),
                "512 blocks",
            ),
        )
        for name, nand, receipt, message in cases:
            with self.subTest(name), self.assertRaisesRegex(SystemExit, message):
                device_data_prepare.dump_page_bytes(TARGET, nand, receipt)


if __name__ == "__main__":
    unittest.main()
