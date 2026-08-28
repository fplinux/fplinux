# SPDX-License-Identifier: GPL-2.0-only
"""Small tests for the MicroPythonOS storage adapter API."""

from __future__ import annotations

import importlib.util
import os
import unittest
from pathlib import Path
from typing import Protocol, cast
from unittest.mock import mock_open, patch

ROOT = Path(__file__).resolve().parents[2]
STORAGE = ROOT / "alpine/aports/fplinux-micropythonos/fplinux_storage.py"


class _StorageCapability(Protocol):
    def mount(self, format: bool = False) -> bool: ...  # noqa: A002, FBT001, FBT002

    def is_mounted(self) -> bool: ...

    def get_mount_point(self) -> str | None: ...

    def get_mode(self) -> str | None: ...


class _StorageModule(Protocol):
    FPLinuxStorage: _StorageCapability


def load_storage_adapter() -> _StorageModule:
    """Load the target-neutral adapter from its shipped source file."""
    specification = importlib.util.spec_from_file_location("fplinux_mpos_storage", STORAGE)
    if specification is None or specification.loader is None:
        message = "cannot load the FPLinux storage adapter"
        raise RuntimeError(message)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return cast("_StorageModule", module)


class MicroPythonOsStorageAdapterTests(unittest.TestCase):
    """Exercise the in-process storage adapter behavior."""

    def test_unreadable_mount_table_hides_storage(self) -> None:
        """The adapter reports no storage when it cannot read mountinfo."""
        storage = load_storage_adapter().FPLinuxStorage

        with (
            patch.dict(os.environ, {"MPOS_STORAGE": "/mnt/card"}, clear=True),
            patch("builtins.open", side_effect=OSError),
        ):
            self.assertFalse(storage.mount())
            self.assertFalse(storage.is_mounted())
            self.assertIsNone(storage.get_mount_point())
            self.assertIsNone(storage.get_mode())

    def test_exact_mountinfo_match_exposes_linux_storage(self) -> None:
        """The adapter exposes only the configured mount point and mode."""
        storage = load_storage_adapter().FPLinuxStorage
        mountinfo = "36 25 179:1 / /mnt/card rw,relatime - vfat /dev/mmcblk0p1 rw\n"

        with (
            patch.dict(os.environ, {"MPOS_STORAGE": "/mnt/card"}, clear=True),
            patch("builtins.open", mock_open(read_data=mountinfo)),
        ):
            self.assertTrue(storage.mount())
            self.assertTrue(storage.is_mounted())
            self.assertEqual(storage.get_mount_point(), "/mnt/card")
            self.assertEqual(storage.get_mode(), "linux")
            self.assertFalse(storage.mount(format=True))


if __name__ == "__main__":
    unittest.main()
