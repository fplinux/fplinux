# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for the bounded Bluetooth firmware preparation command."""

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

from fplinux_cli import bluetooth_prepare

TARGET = "nokia-ta1618"
NAMES = (
    "ta1618-cm4.bin",
    "ta1618-bt-config.bin",
    "ta1618-bt-sprd.bin",
    "ta1618-bt-rf-config.bin",
)
ORIGINALS = {
    "ta1618-cm4.bin": b"fitted CM4",
    "ta1618-bt-config.bin": b"fitted config",
    "ta1618-bt-sprd.bin": b"fitted sprd",
    "ta1618-bt-rf-config.bin": b"fitted radio",
}
PREPARED = {
    "ta1618-cm4.bin": b"prepared CM4",
    "ta1618-bt-config.bin": b"fitted config",
    "ta1618-bt-sprd.bin": b"fitted sprd",
    "ta1618-bt-rf-config.bin": b"fitted radio",
}
RAW_DUMP = b"controlled physical NAND backup"


class BluetoothPrepareTests(unittest.TestCase):
    """Exercise real private-file publication while replacing phone and build boundaries."""

    def setUp(self) -> None:
        """Create one cache and saved dump isolated from ambient firmware inputs."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.cache = self.root / ".cache"
        self.cache.mkdir(parents=True)
        self.saved_dump = self.root / "saved-nand.bin"
        self.saved_dump.write_bytes(RAW_DUMP)
        self.declarations = [
            {
                "source": name,
                "destination": f"ums9117/{name}",
                "size": len(PREPARED[name]),
                **(
                    {"sha256": hashlib.sha256(PREPARED[name]).hexdigest()}
                    if name == "ta1618-cm4.bin"
                    else {}
                ),
            }
            for name in NAMES
        ]

    def _config(self) -> dict[str, object]:
        return {
            "bluetooth": {"parser": "radio_parser.py"},
            "rootfs": {"firmware": self.declarations},
        }

    @staticmethod
    def _parser(
        originals: dict[str, bytes] = ORIGINALS,
        prepared: dict[str, bytes] = PREPARED,
    ) -> SimpleNamespace:
        def extract(_raw: bytes) -> SimpleNamespace:
            return SimpleNamespace(originals=originals, prepared=prepared)

        return SimpleNamespace(prepare_firmware=extract)

    def _run_from_dump(self, *, parser: SimpleNamespace | None = None) -> str:
        with (
            mock.patch.object(bluetooth_prepare, "ROOT", self.root),
            mock.patch.object(bluetooth_prepare, "load_target", return_value=self._config()),
            mock.patch.object(
                bluetooth_prepare,
                "_load_firmware_parser",
                return_value=parser or self._parser(),
            ),
            mock.patch.object(
                bluetooth_prepare,
                "build",
                side_effect=AssertionError("saved-dump preparation must not build"),
            ),
            mock.patch.object(
                bluetooth_prepare,
                "run_target_noninteractive",
                side_effect=AssertionError("saved-dump preparation must not load a phone"),
            ),
            mock.patch.object(
                bluetooth_prepare,
                "backup_target_nand",
                side_effect=AssertionError("saved-dump preparation must not access SSH"),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            bluetooth_prepare.prepare_bluetooth(
                TARGET,
                from_dump=self.saved_dump,
                jobs=3,
                offline=True,
            )
        return stdout.getvalue()

    def test_saved_dump_publishes_exact_private_inputs_and_preserves_originals(self) -> None:
        """One admitted generation replaces current inputs without altering its source evidence."""
        previous = self.cache / "firmware" / TARGET
        previous.mkdir(parents=True)
        for name in NAMES:
            (previous / name).write_bytes(b"previous")

        output = self._run_from_dump()

        self.assertEqual(self.saved_dump.read_bytes(), RAW_DUMP)
        self.assertEqual(previous.stat().st_mode & 0o777, 0o700)
        for name in NAMES:
            published = previous / name
            self.assertEqual(published.read_bytes(), PREPARED[name])
            self.assertEqual(published.stat().st_mode & 0o777, 0o600)

        source_runs = list((previous / "sources").glob("run-*"))
        self.assertEqual(len(source_runs), 1)
        source_run = source_runs[0]
        for name in NAMES:
            original = source_run / "originals" / name
            self.assertEqual(original.read_bytes(), ORIGINALS[name])
            self.assertEqual(original.stat().st_mode & 0o777, 0o600)
        self.assertEqual(list(source_run.glob(".admission-*")), [])

        receipt = json.loads((source_run / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["source"], str(self.saved_dump.absolute()))
        self.assertEqual(receipt["source_sha256"], hashlib.sha256(RAW_DUMP).hexdigest())
        self.assertIn("./fplinux build nokia-ta1618", output)
        self.assertIn("./fplinux run nokia-ta1618", output)
        self.assertNotIn("--profile", output)

    def test_failed_admission_keeps_all_previous_current_inputs(self) -> None:
        """A wrong prepared size fails after preserving originals but before publication."""
        current = self.cache / "firmware" / TARGET
        current.mkdir(parents=True)
        previous = {name: f"previous:{name}".encode() for name in NAMES}
        for name, contents in previous.items():
            path = current / name
            path.write_bytes(contents)
            path.chmod(0o600)
        wrong = {**PREPARED, "ta1618-bt-config.bin": b"wrong size"}

        with self.assertRaisesRegex(SystemExit, "ta1618-bt-config.bin has .* expected"):
            self._run_from_dump(parser=self._parser(prepared=wrong))

        self.assertEqual(self.saved_dump.read_bytes(), RAW_DUMP)
        for name, contents in previous.items():
            self.assertEqual((current / name).read_bytes(), contents)
        source_run = next((current / "sources").glob("run-*"))
        self.assertEqual(
            {path.name: path.read_bytes() for path in (source_run / "originals").iterdir()},
            ORIGINALS,
        )
        self.assertFalse((source_run / "receipt.json").exists())

    def test_live_path_builds_loads_then_backs_up_before_extraction(self) -> None:
        """The live workflow follows loader-first NAND access without recursive CLI dispatch."""
        events: list[tuple[object, ...]] = []

        def backup(target: str, destination: Path) -> Path:
            events.append(("backup", target, destination))
            destination.write_bytes(RAW_DUMP)
            destination.chmod(0o600)
            return destination

        def extract(raw: bytes) -> SimpleNamespace:
            events.append(("extract", raw))
            return SimpleNamespace(originals=ORIGINALS, prepared=PREPARED)

        with (
            mock.patch.object(bluetooth_prepare, "ROOT", self.root),
            mock.patch.object(bluetooth_prepare, "load_target", return_value=self._config()),
            mock.patch.object(
                bluetooth_prepare,
                "build",
                side_effect=lambda *args, **kwargs: events.append(("build", args, kwargs)),
            ),
            mock.patch.object(
                bluetooth_prepare,
                "run_target_noninteractive",
                side_effect=lambda *args, **kwargs: events.append(("load", args, kwargs)),
            ),
            mock.patch.object(bluetooth_prepare, "backup_target_nand", side_effect=backup),
            mock.patch.object(
                bluetooth_prepare,
                "_load_firmware_parser",
                return_value=SimpleNamespace(prepare_firmware=extract),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            bluetooth_prepare.prepare_bluetooth(
                TARGET,
                from_dump=None,
                jobs=2,
                offline=True,
            )

        self.assertEqual([event[0] for event in events], ["build", "load", "backup", "extract"])
        self.assertEqual(
            events[0],
            ("build", (TARGET, 2), {"offline": True}),
        )
        self.assertEqual(
            events[1],
            ("load", (TARGET,), {"profile": None}),
        )
        source_run = next((self.cache / "firmware" / TARGET / "sources").glob("run-*"))
        raw = source_run / "nand.bin"
        self.assertEqual(raw.read_bytes(), RAW_DUMP)
        self.assertEqual(raw.stat().st_mode & 0o777, 0o600)
        progress = stdout.getvalue()
        for stage in ("build", "load", "connect", "dump", "extract", "ready"):
            self.assertIn(stage, progress.lower())

    def test_selected_target_parser_is_loaded_from_its_declared_filename(self) -> None:
        """The declared board parser produces the published input and preserved original."""
        target = "another-board"
        directory = self.root / "targets" / target
        directory.mkdir(parents=True)
        (directory / "radio_parser.py").write_text(
            "from types import SimpleNamespace\n"
            "def prepare_firmware(raw):\n"
            "    return SimpleNamespace(originals={'radio.bin': raw}, "
            "prepared={'radio.bin': raw[::-1]})\n"
        )
        self.saved_dump.write_bytes(b"radio")
        config = {
            "bluetooth": {"parser": "radio_parser.py"},
            "rootfs": {
                "firmware": [
                    {"source": "radio.bin", "destination": "example/radio.bin", "size": 5}
                ]
            },
        }
        with (
            mock.patch.object(bluetooth_prepare, "ROOT", self.root),
            mock.patch.object(bluetooth_prepare, "load_target", return_value=config),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            bluetooth_prepare.prepare_bluetooth(
                target, from_dump=self.saved_dump, jobs=1, offline=True
            )
        published = self.cache / "firmware" / target
        self.assertEqual((published / "radio.bin").read_bytes(), b"oidar")
        source_run = next((published / "sources").glob("run-*"))
        self.assertEqual((source_run / "originals/radio.bin").read_bytes(), b"radio")
        self.assertEqual(self.saved_dump.read_bytes(), b"radio")

    def test_board_without_bluetooth_stops_before_build_or_source_access(self) -> None:
        """No declaration means no supported preparation or firmware directory."""
        with (
            mock.patch.object(bluetooth_prepare, "ROOT", self.root),
            mock.patch.object(
                bluetooth_prepare, "load_target", return_value={"rootfs": {"firmware": []}}
            ),
            mock.patch.object(bluetooth_prepare, "build") as build,
            self.assertRaisesRegex(SystemExit, "not supported for target another-board"),
        ):
            bluetooth_prepare.prepare_bluetooth(
                "another-board", from_dump=None, jobs=1, offline=True
            )
        build.assert_not_called()
        self.assertFalse((self.cache / "firmware").exists())


if __name__ == "__main__":
    unittest.main()
