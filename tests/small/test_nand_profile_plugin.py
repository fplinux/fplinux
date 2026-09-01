# SPDX-License-Identifier: GPL-2.0-only
"""Small publication checks for the read-only NAND profile plugin."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any, BinaryIO
from unittest import mock

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / "targets/nokia-ta1618/profiles/nand-ro-lab/host_plugin.py"


def load_plugin() -> ModuleType:
    """Load the production plugin without copying its implementation into the test."""
    spec = importlib.util.spec_from_file_location("test_nand_ro_lab_plugin_small", PLUGIN)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load profile plugin: {PLUGIN}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NandProfilePluginTests(unittest.TestCase):
    """Protect the physical image format and atomic publication result."""

    def setUp(self) -> None:
        """Create one isolated destination and fresh plugin module."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.destination = self.directory / "nand.raw"
        self.plugin = load_plugin()

    def test_physical_image_size_is_exact(self) -> None:
        """The completed backup contains every physical main and OOB byte."""
        self.assertEqual(self.plugin.RAW_BYTES, 142606336)

    def test_complete_stream_is_atomically_published(self) -> None:
        """Only an exact completed stream replaces the requested output."""
        payload = bytes(range(256)) * 16

        def stream_remote(
            _session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
        ) -> None:
            self.assertTrue(command.startswith("exec dd if=/dev/ta1618-nand-raw "))
            del timeout
            destination.write(payload)

        ssh = SimpleNamespace(stream_remote=stream_remote)
        with (
            mock.patch.object(self.plugin, "RAW_BYTES", len(payload)),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            self.plugin.backup(ssh, {}, str(self.destination))

        self.assertEqual(self.destination.read_bytes(), payload)
        self.assertIn(self.plugin.sha256_file(self.destination), stdout.getvalue())
        self.assertEqual(list(self.directory.glob(".nand.raw.*")), [])

    def test_plugin_help_does_not_connect_to_the_phone(self) -> None:
        """Argument help is available before any SSH session is reacquired."""
        connect = mock.Mock()
        with (
            contextlib.redirect_stdout(io.StringIO()) as stdout,
            self.assertRaises(SystemExit) as stopped,
        ):
            self.plugin.run(connect, ["--help"])

        self.assertEqual(stopped.exception.code, 0)
        self.assertIn("nand-backup", stdout.getvalue())
        connect.assert_not_called()


if __name__ == "__main__":
    unittest.main()
