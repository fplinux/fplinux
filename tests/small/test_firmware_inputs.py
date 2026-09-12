# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for declared local firmware in immutable rootfs builds."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state, config, firmware_inputs
from fplinux_cli import workspace as workspace_module


class FirmwareInputTests(unittest.TestCase):
    """Keep admission, cache identity and rootfs installation bound to exact bytes."""

    target = "demo"

    def setUp(self) -> None:
        """Create one isolated target firmware directory for each test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        self.input_directory = self.cache / "firmware" / self.target
        self.input_directory.mkdir(parents=True)

    @staticmethod
    def _declaration(
        source: str = "controller.bin",
        destination: str = "chip/controller.bin",
        size: int = 8,
        sha256: str | None = None,
    ) -> dict[str, object]:
        declaration: dict[str, object] = {
            "source": source,
            "destination": destination,
            "size": size,
        }
        if sha256 is not None:
            declaration["sha256"] = sha256
        return declaration

    def _capture(
        self, declarations: list[dict[str, object]]
    ) -> tuple[firmware_inputs.FirmwareInput, ...]:
        return firmware_inputs.capture_external_firmware_inputs(
            self.target,
            declarations,
            self.cache,
        )

    def test_target_firmware_schema_accepts_safe_records_and_rejects_bad_destinations(
        self,
    ) -> None:
        """The target contract names files below /lib/firmware exactly once."""
        contents = b"firmware"
        digest = hashlib.sha256(contents).hexdigest()
        normalized = config.firmware_array(
            [self._declaration(sha256=digest)],
            "target bluetooth firmware",
        )

        self.assertEqual(
            normalized,
            [
                {
                    "source": "controller.bin",
                    "destination": "chip/controller.bin",
                    "size": 8,
                    "sha256": digest,
                }
            ],
        )

        invalid_cases = {
            "source directories": [self._declaration(source="private/controller.bin")],
            "escaping destinations": [self._declaration(destination="../controller.bin")],
            "directory destinations": [self._declaration(destination=".")],
            "duplicate destinations": [self._declaration(), self._declaration(source="other.bin")],
            "duplicate sources": [
                self._declaration(),
                self._declaration(destination="chip/other.bin"),
            ],
            "zero sizes": [self._declaration(size=0)],
        }
        for name, declarations in invalid_cases.items():
            with self.subTest(name=name), self.assertRaises(SystemExit):
                config.firmware_array(declarations, "target bluetooth firmware")

    def test_declared_set_rejects_missing_partial_and_hash_mismatched_files(self) -> None:
        """Admission rejects incomplete, wrong-size and hash-mismatched input sets."""
        (self.input_directory / "controller.bin").write_bytes(b"firmware")
        missing = [
            self._declaration(),
            self._declaration("fitted.bin", "chip/fitted.bin", 4),
        ]
        with self.assertRaisesRegex(SystemExit, "fitted.bin"):
            self._capture(missing)

        with self.assertRaisesRegex(SystemExit, "has 8 bytes; expected 9"):
            self._capture([self._declaration(size=9)])

        with self.assertRaisesRegex(SystemExit, "SHA-256 is .* expected 0{64}"):
            self._capture([self._declaration(sha256="0" * 64)])

    def test_target_without_firmware_declarations_needs_no_private_directory(self) -> None:
        """A target without declarations retains the ordinary input closure."""
        self.input_directory.rmdir()
        (self.cache / "firmware").rmdir()
        self.cache.rmdir()

        self.assertEqual(
            firmware_inputs.capture_external_firmware_inputs(self.target, [], self.cache),
            (),
        )

    def test_entire_declared_group_may_be_absent_from_live_and_snapshot_inputs(self) -> None:
        """A first build remains available until any fitted firmware file is supplied."""
        (self.input_directory / "sources").mkdir()
        declarations = [
            self._declaration(),
            self._declaration("fitted.bin", "chip/fitted.bin", 4),
        ]

        self.assertEqual(self._capture(declarations), ())
        self.assertEqual(
            firmware_inputs.capture_snapshot_firmware_inputs(
                self.target,
                declarations,
                self.root / "fresh-workspace",
            ),
            (),
        )

    def test_selected_bytes_are_causal_and_snapshot_staging_is_immutable(self) -> None:
        """Only selected bytes alter the workspace, which stages the captured generation."""
        selected = self.input_directory / "controller.bin"
        selected.write_bytes(b"firmware")
        declaration = [self._declaration()]
        target_config = {"rootfs": {"firmware": declaration}}
        base = self.root / "base-source"
        base.write_bytes(b"base")

        with (
            mock.patch.object(workspace_module, "ROOT", self.root),
            mock.patch.object(workspace_module, "load_target", return_value=target_config),
            mock.patch.object(
                workspace_module,
                "target_build_source_files",
                return_value=[("base-source", base)],
            ),
        ):
            first = workspace_module.target_workspace_snapshot(self.target)
            unchanged = workspace_module.target_workspace_snapshot(self.target)
            (self.input_directory / "unrelated.bin").write_bytes(b"ignored")
            unrelated = workspace_module.target_workspace_snapshot(self.target)
            selected.write_bytes(b"changed!")
            changed = workspace_module.target_workspace_snapshot(self.target)
            staged = workspace_module.stage_workspace_snapshot(first)

        self.assertEqual(first.recipe, unchanged.recipe)
        self.assertEqual(first.recipe, unrelated.recipe)
        self.assertNotEqual(first.recipe, changed.recipe)
        staged_input = staged / firmware_inputs.snapshot_firmware_path(
            self.target, "controller.bin"
        )
        self.assertEqual(staged_input.read_bytes(), b"firmware")
        self.assertEqual(staged_input.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(".cache", staged_input.relative_to(staged).parts)

    def test_rootfs_receipt_tracks_selected_bytes_but_not_adjacent_files(self) -> None:
        """The rootfs cache hits only for the exact admitted firmware generation."""
        selected = self.input_directory / "controller.bin"
        selected.write_bytes(b"firmware")
        declaration = [self._declaration()]
        captured = self._capture(declaration)
        first_recipe = alpine_state.alpine_rootfs_recipe(
            "1" * 64,
            "2" * 64,
            ("fplinux-base",),
            firmware_inputs=captured,
        )
        output = alpine_state.rootfs_output(self.root / "state", first_recipe)
        output.mkdir(parents=True)
        (output / alpine_state.ROOTFS_NAME).write_bytes(b"rootfs")
        alpine_state.write_receipt(output, first_recipe)

        self.assertTrue(alpine_state.receipt_matches(output, first_recipe))
        (self.input_directory / "unrelated.bin").write_bytes(b"ignored")
        unrelated_recipe = alpine_state.alpine_rootfs_recipe(
            "1" * 64,
            "2" * 64,
            ("fplinux-base",),
            firmware_inputs=self._capture(declaration),
        )
        self.assertEqual(unrelated_recipe, first_recipe)
        self.assertTrue(alpine_state.receipt_matches(output, unrelated_recipe))

        selected.write_bytes(b"changed!")
        changed_recipe = alpine_state.alpine_rootfs_recipe(
            "1" * 64,
            "2" * 64,
            ("fplinux-base",),
            firmware_inputs=self._capture(declaration),
        )
        self.assertNotEqual(changed_recipe, first_recipe)
        self.assertFalse(alpine_state.receipt_matches(output, changed_recipe))

    def test_rootfs_installs_captured_bytes_at_declared_path_with_private_mode(self) -> None:
        """The consumer copies the captured generation, not a later live-file mutation."""
        source = self.input_directory / "controller.bin"
        source.write_bytes(b"firmware")
        captured = self._capture([self._declaration()])
        source.write_bytes(b"changed!")
        rootfs = self.root / "rootfs"
        (rootfs / "lib").mkdir(parents=True)

        firmware_inputs.install_firmware_inputs(rootfs, captured)
        firmware_inputs.verify_installed_firmware_inputs(rootfs, captured)

        installed = rootfs / "lib/firmware/chip/controller.bin"
        self.assertEqual(installed.read_bytes(), b"firmware")
        self.assertEqual(installed.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
