# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for generic MicroPythonOS source-pruning behavior.

These tests use sentinel application names and satisfy the selector's required-logo
precondition. They do not copy the shipped application or library inventory and do
not claim to validate the packaged source closure.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]
SELECTOR = ROOT / "alpine/aports/fplinux-micropythonos/fplinux-select-builtin.py"


def load_selector() -> ModuleType:
    """Load the real builtin-pruning implementation."""
    specification = importlib.util.spec_from_file_location("mpos_builtin_selector_test", SELECTOR)
    if specification is None or specification.loader is None:
        message = "cannot load builtin selector"
        raise RuntimeError(message)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def make_builtin_root(root: Path, *app_names: str) -> Path:
    """Create the minimum generic builtin tree required by the selector."""
    apps = root / "internal_filesystem/builtin/apps"
    for name in app_names:
        (apps / name).mkdir(parents=True)
    logo = root / "internal_filesystem/builtin/res/MicroPythonOS-logo-white-long-w296.png"
    logo.parent.mkdir(parents=True)
    logo.touch()
    return apps


class MicroPythonOsSelectorTests(unittest.TestCase):
    """Exercise generic pruning and missing-input behavior with sentinels."""

    def test_builtin_selector_removes_unselected_sentinels(self) -> None:
        """Keep the allowlisted app and remove unrelated sentinel content."""
        selector = load_selector()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apps = make_builtin_root(root, "keep-app", "drop-app")

            selector.select_builtin(root, {"keep-app"})

            self.assertTrue((apps / "keep-app").is_dir())
            self.assertFalse((apps / "drop-app").exists())

    def test_builtin_selector_rejects_missing_allowlisted_sentinel(self) -> None:
        """Fail when a requested application is absent from the source tree."""
        selector = load_selector()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_builtin_root(root, "present-app")

            with self.assertRaisesRegex(FileNotFoundError, "missing builtin apps"):
                selector.select_builtin(root, {"missing-app"})


if __name__ == "__main__":
    unittest.main()
