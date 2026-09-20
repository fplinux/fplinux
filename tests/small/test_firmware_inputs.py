# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for named device-data groups in immutable builds."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state, config, firmware_inputs
from fplinux_cli import workspace as workspace_module


class FirmwareInputTests(unittest.TestCase):
    """Keep optional groups, build identity, and their consumers explicit."""

    target = "demo"

    def setUp(self) -> None:
        """Create one isolated selected generation for each test."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        target_root = self.cache / "device-data" / self.target
        self.generation = target_root / "generations/generation-test"
        (self.generation / "groups").mkdir(parents=True)
        (target_root / "current").write_text("generation-test\n", encoding="ascii")
        self.groups: dict[str, list[dict[str, object]]] = {
            "bluetooth": [
                self._declaration(
                    "controller.bin",
                    "chip/controller.bin",
                    8,
                )
            ],
            "audio-profile": [
                self._declaration(
                    "profile.bin",
                    "fplinux/profile.bin",
                    7,
                )
            ],
        }

    @staticmethod
    def _declaration(
        source: str,
        destination: str,
        size: int,
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

    def _write_group(self, name: str, files: dict[str, bytes]) -> Path:
        directory = self.generation / "groups" / name
        directory.mkdir()
        for filename, contents in files.items():
            (directory / filename).write_bytes(contents)
        return directory

    def _capture(self) -> dict[str, tuple[firmware_inputs.FirmwareInput, ...]]:
        return firmware_inputs.capture_external_device_data(
            self.target,
            self.groups,
            self.cache,
        )

    def test_firmware_schema_accepts_safe_records_and_rejects_bad_paths(self) -> None:
        """Each group declaration names one source and one firmware destination."""
        contents = b"firmware"
        digest = hashlib.sha256(contents).hexdigest()
        normalized = config.firmware_array(
            [self._declaration("controller.bin", "chip/controller.bin", 8, digest)],
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
            "source directories": [
                self._declaration("private/controller.bin", "chip/controller.bin", 8)
            ],
            "escaping destinations": [self._declaration("controller.bin", "../controller.bin", 8)],
            "duplicate destinations": [
                self._declaration("controller.bin", "chip/controller.bin", 8),
                self._declaration("other.bin", "chip/controller.bin", 8),
            ],
            "zero sizes": [self._declaration("controller.bin", "chip/controller.bin", 0)],
        }
        for name, declarations in invalid_cases.items():
            with self.subTest(name=name), self.assertRaises(SystemExit):
                config.firmware_array(declarations, "target bluetooth firmware")

    def test_absent_groups_are_independent_and_partial_group_names_its_error(self) -> None:
        """Whole optional groups may be absent, but a present group is all-or-nothing."""
        self._write_group("bluetooth", {"controller.bin": b"firmware"})

        captured = self._capture()

        self.assertEqual(tuple(captured), ("bluetooth",))
        self.assertEqual(captured["bluetooth"][0].contents, b"firmware")
        self.groups["audio-profile"] = [
            self._declaration("profile.bin", "fplinux/profile.bin", 7),
            self._declaration("second.bin", "fplinux/second.bin", 3),
        ]
        self._write_group("audio-profile", {"profile.bin": b"profile"})
        with self.assertRaisesRegex(SystemExit, "device-data group audio-profile:.*second.bin"):
            self._capture()

    def test_old_firmware_layout_is_a_cache_miss(self) -> None:
        """An obsolete direct-file cache is ignored without a compatibility reader."""
        target_root = self.cache / "device-data" / self.target
        (target_root / "current").unlink()
        old = self.cache / "firmware" / self.target
        old.mkdir(parents=True)
        (old / "controller.bin").write_bytes(b"firmware")

        self.assertEqual(self._capture(), {})

    def test_profile_bytes_are_causal_but_unrelated_generation_files_are_not(self) -> None:
        """The workspace recipe follows declared profile bytes and ignores adjacent files."""
        self._write_group("bluetooth", {"controller.bin": b"firmware"})
        audio = self._write_group("audio-profile", {"profile.bin": b"profile"})
        target_config = {"device_data": {"groups": self.groups}}
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
            (audio / "unrelated.bin").write_bytes(b"ignored")
            unrelated = workspace_module.target_workspace_snapshot(self.target)
            (audio / "profile.bin").write_bytes(b"changed")
            changed = workspace_module.target_workspace_snapshot(self.target)
            staged = workspace_module.stage_workspace_snapshot(first)

        self.assertEqual(first.recipe, unrelated.recipe)
        self.assertNotEqual(first.recipe, changed.recipe)
        staged_profile = staged / (
            ".fplinux-inputs/device-data/demo/groups/audio-profile/fplinux/profile.bin"
        )
        self.assertEqual(staged_profile.read_bytes(), b"profile")
        self.assertEqual(staged_profile.stat().st_mode & 0o777, 0o600)
        self.assertNotIn(".cache", staged_profile.relative_to(staged).parts)

    def test_rootfs_receipt_depends_on_bluetooth_not_audio_or_adjacent_files(self) -> None:
        """Rootfs reuse follows Bluetooth bytes, independently of the kernel-only profile."""
        bluetooth = self._write_group("bluetooth", {"controller.bin": b"firmware"})
        audio = self._write_group("audio-profile", {"profile.bin": b"profile"})

        def rootfs_recipe() -> str:
            return alpine_state.alpine_rootfs_recipe(
                "1" * 64,
                "2" * 64,
                ("fplinux-base",),
                firmware_inputs=self._capture()["bluetooth"],
            )

        first = rootfs_recipe()
        output = alpine_state.rootfs_output(self.root / "state", first)
        output.mkdir(parents=True)
        (output / alpine_state.ROOTFS_NAME).write_bytes(b"rootfs")
        alpine_state.write_receipt(output, first)
        self.assertTrue(alpine_state.receipt_matches(output, first))

        (bluetooth / "unrelated.bin").write_bytes(b"ignored")
        adjacent_changed = rootfs_recipe()
        self.assertEqual(adjacent_changed, first)
        self.assertTrue(alpine_state.receipt_matches(output, adjacent_changed))

        (audio / "profile.bin").write_bytes(b"changed")
        audio_changed = rootfs_recipe()
        self.assertEqual(audio_changed, first)
        self.assertTrue(alpine_state.receipt_matches(output, audio_changed))

        (bluetooth / "controller.bin").write_bytes(b"changed!")
        bluetooth_changed = rootfs_recipe()
        self.assertNotEqual(bluetooth_changed, first)
        self.assertFalse(alpine_state.receipt_matches(output, bluetooth_changed))

    def test_installer_writes_exact_bytes_with_private_mode(self) -> None:
        """The installer writes an admitted firmware input directly with mode 0600."""
        self._write_group("bluetooth", {"controller.bin": b"firmware"})
        rootfs_firmware = self._capture()["bluetooth"]
        rootfs = self.root / "rootfs"
        (rootfs / "lib").mkdir(parents=True)

        firmware_inputs.install_firmware_inputs(rootfs, rootfs_firmware)

        installed = rootfs / "lib/firmware/chip/controller.bin"
        self.assertEqual(installed.read_bytes(), b"firmware")
        self.assertEqual(installed.stat().st_mode & 0o777, 0o600)

    def test_installed_firmware_verifier_rejects_changed_bytes_and_mode(self) -> None:
        """Verification rejects either material corruption of a controlled destination."""
        self._write_group("bluetooth", {"controller.bin": b"firmware"})
        rootfs_firmware = self._capture()["bluetooth"]
        cases = (
            ("changed bytes", b"changed!", 0o600, "bytes do not match"),
            ("public mode", b"firmware", 0o644, "mode is 0644, expected 0600"),
        )
        for name, contents, mode, error in cases:
            with self.subTest(name=name):
                rootfs = self.root / name.replace(" ", "-")
                installed = rootfs / "lib/firmware/chip/controller.bin"
                installed.parent.mkdir(parents=True)
                installed.write_bytes(contents)
                installed.chmod(mode)

                with self.assertRaisesRegex(SystemExit, error):
                    firmware_inputs.verify_installed_firmware_inputs(
                        rootfs,
                        rootfs_firmware,
                    )


if __name__ == "__main__":
    unittest.main()
