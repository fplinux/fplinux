# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for named device-data groups in immutable builds."""

from __future__ import annotations

import hashlib
import lzma
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.alpine import recipes as alpine_recipes
from fplinux_cli.alpine import rootfs_state as alpine_state
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.manifests import values
from fplinux_cli.workspace import build_inputs as workspace_module
from fplinux_cli.workspace import staging as workspace_staging

from tests.small.workspace.workspace_fixtures import empty_package_graph, write_source_lock_files

if TYPE_CHECKING:
    from pathlib import Path


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


@dataclass(frozen=True)
class FirmwareInputs:
    """Selected phone data and mutable declarations owned by one case."""

    root: Path
    phone: Path
    groups: dict[str, list[dict[str, object]]]
    target: str

    def write_group(self, name: str, files: dict[str, bytes]) -> Path:
        """Populate one selected group's controlled input bytes."""
        directory = self.phone / "groups" / name
        directory.mkdir()
        for filename, contents in files.items():
            (directory / filename).write_bytes(contents)
        return directory

    def capture(self) -> dict[str, tuple[firmware_inputs.FirmwareInput, ...]]:
        """Capture admitted inputs through the production flat-store consumer."""
        return firmware_inputs.capture_external_device_data(
            self.target,
            self.groups,
        )


@pytest.fixture
def firmware_generation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> FirmwareInputs:
    """Own one isolated selected phone store until teardown."""
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    target = "demo"
    phone = tmp_path / "data/fplinux/phones/demo/phone"
    (phone / "groups").mkdir(parents=True)
    groups: dict[str, list[dict[str, object]]] = {
        "bluetooth": [_declaration("controller.bin", "chip/controller.bin", 8)],
        "audio-profile": [_declaration("profile.bin", "fplinux/profile.bin", 7)],
    }
    return FirmwareInputs(tmp_path, phone, groups, target)


class FirmwareInputTests:
    """Keep optional groups, build identity, and their consumers explicit."""

    @pytest.mark.parametrize(
        "case",
        ["source directories", "escaping destinations", "duplicate destinations", "zero sizes"],
        ids=["source-directory", "escaping-destination", "duplicate-destination", "zero-size"],
    )
    def test_firmware_schema_accepts_safe_records_and_rejects_bad_paths(self, case: str) -> None:
        """Each group declaration names one source and one firmware destination."""
        contents = b"firmware"
        digest = hashlib.sha256(contents).hexdigest()
        normalized = values.firmware_array(
            [_declaration("controller.bin", "chip/controller.bin", 8, digest)],
            "target bluetooth firmware",
        )

        assert (normalized) == (
            [
                {
                    "source": "controller.bin",
                    "destination": "chip/controller.bin",
                    "size": 8,
                    "sha256": digest,
                }
            ]
        )
        invalid_cases = {
            "source directories": [
                _declaration("private/controller.bin", "chip/controller.bin", 8)
            ],
            "escaping destinations": [_declaration("controller.bin", "../controller.bin", 8)],
            "duplicate destinations": [
                _declaration("controller.bin", "chip/controller.bin", 8),
                _declaration("other.bin", "chip/controller.bin", 8),
            ],
            "zero sizes": [_declaration("controller.bin", "chip/controller.bin", 0)],
        }
        with pytest.raises(SystemExit):
            values.firmware_array(invalid_cases[case], "target bluetooth firmware")

    def test_absent_groups_are_independent_and_partial_group_names_its_error(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """Whole optional groups may be absent, but a present group is all-or-nothing."""
        firmware_generation.write_group("bluetooth", {"controller.bin": b"firmware"})

        captured = firmware_generation.capture()

        assert (tuple(captured)) == (("bluetooth",))
        assert (captured["bluetooth"][0].contents) == (b"firmware")
        firmware_generation.groups["audio-profile"] = [
            _declaration("profile.bin", "fplinux/profile.bin", 7),
            _declaration("second.bin", "fplinux/second.bin", 3),
        ]
        firmware_generation.write_group("audio-profile", {"profile.bin": b"profile"})
        with pytest.raises(SystemExit, match=r"device-data group audio-profile:.*second.bin"):
            firmware_generation.capture()

    def test_checkout_local_phone_data_is_ignored_without_automatic_migration(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """Only the persistent store supplies inputs; old checkout directories stay untouched."""
        shutil.rmtree(firmware_generation.phone)
        old = firmware_generation.root / ".cache/device-data/demo/phone/groups/bluetooth"
        old.mkdir(parents=True)
        (old / "controller.bin").write_bytes(b"firmware")

        assert (firmware_generation.capture()) == ({})
        assert (old / "controller.bin").read_bytes() == b"firmware"

    @pytest.mark.parametrize(
        ("platform", "root_kind", "personal_data_is_build_input"),
        [
            pytest.param("ums9117", "initramfs", False, id="ums-ram-runtime-data"),
            pytest.param("ums9117", "external", True, id="ums-external-build-data"),
            pytest.param("sc9820e", "initramfs", False, id="sc9820e-ram-runtime-data"),
            pytest.param("sc9820e", "external", True, id="sc9820e-external-build-data"),
        ],
    )
    def test_profile_bytes_affect_only_builds_that_deliver_personal_data(
        self,
        firmware_generation: FirmwareInputs,
        platform: str,
        root_kind: str,
        *,
        personal_data_is_build_input: bool,
    ) -> None:
        """Runtime capsules leave build identity fixed; rootfs delivery captures private inputs."""
        firmware_generation.write_group("bluetooth", {"controller.bin": b"firmware"})
        audio = firmware_generation.write_group("audio-profile", {"profile.bin": b"profile"})
        target_config = {
            "platform": platform,
            "uboot": {"kind": "none"},
            "linux": {"root": {"kind": root_kind}},
            "device_data": {"groups": firmware_generation.groups},
        }
        base = firmware_generation.root / "base-source"
        base.write_bytes(b"base")
        registration = firmware_generation.root / "scripts/fplinux_cli/alpine/registration.py"
        registration.parent.mkdir(parents=True)
        registration.write_bytes(b"# empty package declarations\n")
        source_locks = [
            (relative.replace("platforms/demo/", f"platforms/{platform}/"), source)
            for relative, source in write_source_lock_files(
                firmware_generation.root, firmware_generation.target
            )
        ]

        with (
            mock.patch.object(workspace_module, "ROOT", firmware_generation.root),
            mock.patch.object(workspace_staging, "ROOT", firmware_generation.root),
            mock.patch.object(workspace_module, "load_target", return_value=target_config),
            mock.patch.object(workspace_module, "load_platform", return_value={}),
            empty_package_graph(),
            mock.patch.object(
                workspace_module,
                "shared_linux_source_files",
                return_value=workspace_module.SharedLinuxSources(),
            ),
            mock.patch.object(
                workspace_module,
                "target_build_source_files",
                return_value=[
                    ("base-source", base),
                    ("scripts/fplinux_cli/alpine/registration.py", registration),
                    *source_locks,
                ],
            ),
        ):
            first = workspace_module.target_workspace_snapshot(firmware_generation.target)
            (audio / "unrelated.bin").write_bytes(b"ignored")
            unrelated = workspace_module.target_workspace_snapshot(firmware_generation.target)
            (audio / "profile.bin").write_bytes(b"changed")
            changed = workspace_module.target_workspace_snapshot(firmware_generation.target)
            staged = workspace_staging.stage_workspace_snapshot(first)
            other_store = firmware_generation.root / "other-phone"
            shutil.copytree(firmware_generation.phone.parent, other_store)
            (other_store / "phone/groups/audio-profile/profile.bin").write_bytes(b"custom!")
            other_phone = workspace_module.target_workspace_snapshot(
                firmware_generation.target, phone_data=other_store
            )
            staged_other = workspace_staging.stage_workspace_snapshot(other_phone)

        assert (first.recipe) == (unrelated.recipe)
        assert (first.recipe != changed.recipe) is personal_data_is_build_input
        assert (changed.recipe != other_phone.recipe) is personal_data_is_build_input
        staged_profile = staged / (
            ".fplinux-inputs/device-data/demo/groups/audio-profile/fplinux/profile.bin"
        )
        if personal_data_is_build_input:
            assert (staged_profile.read_bytes()) == (b"profile")
            assert (staged_profile.stat().st_mode & 0o777) == (0o600)
            assert (".cache") not in (staged_profile.relative_to(staged).parts)
            assert (
                staged_other
                / ".fplinux-inputs/device-data/demo/groups/audio-profile/fplinux/profile.bin"
            ).read_bytes() == b"custom!"
        else:
            assert not staged_profile.exists()

    def test_rootfs_receipt_depends_on_bluetooth_not_audio_or_adjacent_files(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """Rootfs reuse follows Bluetooth bytes, independently of the kernel-only profile."""
        bluetooth = firmware_generation.write_group("bluetooth", {"controller.bin": b"firmware"})
        audio = firmware_generation.write_group("audio-profile", {"profile.bin": b"profile"})

        def rootfs_recipe() -> str:
            return alpine_recipes.alpine_rootfs_recipe(
                "1" * 64,
                "2" * 64,
                ("fplinux-base",),
                release_id="012345abcdef",
                firmware_inputs=firmware_inputs.rootfs_firmware_inputs(
                    firmware_generation.capture()
                ),
            )

        first = rootfs_recipe()
        output = alpine_state.rootfs_output(firmware_generation.root / "state", first)
        output.mkdir(parents=True)
        (output / alpine_state.ROOTFS_NAME).write_bytes(b"rootfs")
        (output / "initramfs.cpio").write_bytes(b"boot archive")
        alpine_state.write_receipt(output, first)
        assert alpine_state.receipt_matches(output, first)

        (bluetooth / "unrelated.bin").write_bytes(b"ignored")
        adjacent_changed = rootfs_recipe()
        assert (adjacent_changed) == (first)
        assert alpine_state.receipt_matches(output, adjacent_changed)

        (audio / "profile.bin").write_bytes(b"changed")
        audio_changed = rootfs_recipe()
        assert (audio_changed) == (first)
        assert alpine_state.receipt_matches(output, audio_changed)

        (bluetooth / "controller.bin").write_bytes(b"changed!")
        bluetooth_changed = rootfs_recipe()
        assert (bluetooth_changed) != (first)
        assert not (alpine_state.receipt_matches(output, bluetooth_changed))

    def test_installer_writes_exact_bytes_with_private_mode(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """The installer writes an admitted firmware input directly with mode 0600."""
        firmware_generation.write_group("bluetooth", {"controller.bin": b"firmware"})
        rootfs_firmware = firmware_generation.capture()["bluetooth"]
        rootfs = firmware_generation.root / "rootfs"
        (rootfs / "lib").mkdir(parents=True)

        firmware_inputs.install_firmware_inputs(rootfs, rootfs_firmware)

        installed = rootfs / "lib/firmware/chip/controller.bin"
        assert (installed.read_bytes()) == (b"firmware")
        assert (installed.stat().st_mode & 0o777) == (0o600)

    def test_radio_image_and_calibration_reach_the_rootfs_and_change_its_receipt(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """Both radio inputs are available to the firmware loader and invalidate rootfs reuse."""
        firmware_generation.groups["wcn-firmware"] = [
            _declaration("radio.bin", "chip/radio.bin", 5),
            _declaration("calibration.bin", "chip/calibration.bin", 4),
        ]
        radio = firmware_generation.write_group(
            "wcn-firmware", {"radio.bin": b"image", "calibration.bin": b"cali"}
        )
        captured = firmware_inputs.rootfs_firmware_inputs(firmware_generation.capture())
        first = alpine_recipes.alpine_rootfs_recipe(
            "1" * 64,
            "2" * 64,
            ("fplinux-base",),
            release_id="012345abcdef",
            firmware_inputs=captured,
        )
        rootfs = firmware_generation.root / "radio-rootfs"
        rootfs.mkdir()

        firmware_inputs.install_firmware_inputs(rootfs, captured)

        assert (rootfs / "lib/firmware/chip/radio.bin").read_bytes() == b"image"
        assert (rootfs / "lib/firmware/chip/calibration.bin").read_bytes() == b"cali"
        (radio / "calibration.bin").write_bytes(b"new!")
        changed = alpine_recipes.alpine_rootfs_recipe(
            "1" * 64,
            "2" * 64,
            ("fplinux-base",),
            release_id="012345abcdef",
            firmware_inputs=firmware_inputs.rootfs_firmware_inputs(firmware_generation.capture()),
        )
        assert changed != first

    @pytest.mark.parametrize(
        ("contents", "mode", "error"),
        [
            pytest.param(b"changed!", 0o600, "bytes do not match", id="changed-bytes"),
            pytest.param(b"firmware", 0o644, "mode is 0644, expected 0600", id="public-mode"),
        ],
    )
    def test_installed_firmware_verifier_rejects_changed_bytes_and_mode(
        self, firmware_generation: FirmwareInputs, contents: bytes, mode: int, error: str
    ) -> None:
        """Verification rejects either material corruption of a controlled destination."""
        firmware_generation.write_group("bluetooth", {"controller.bin": b"firmware"})
        rootfs_firmware = firmware_generation.capture()["bluetooth"]
        rootfs = firmware_generation.root / "rootfs"
        installed = rootfs / "lib/firmware/chip/controller.bin"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(contents)
        installed.chmod(mode)

        with pytest.raises(SystemExit, match=error):
            firmware_inputs.verify_installed_firmware_inputs(rootfs, rootfs_firmware)

    def test_compressed_firmware_roundtrips_with_private_mode(
        self, firmware_generation: FirmwareInputs
    ) -> None:
        """The kernel XZ fallback supplies admitted bytes without a plain duplicate."""
        contents = bytes(range(256)) * 32
        firmware = firmware_inputs.FirmwareInput(
            source="controller.bin",
            destination="chip/controller.bin",
            contents=contents,
            sha256=hashlib.sha256(contents).hexdigest(),
        )
        rootfs = firmware_generation.root / "compressed-rootfs"
        rootfs.mkdir()

        firmware_inputs.install_firmware_inputs(rootfs, (firmware,))

        installed = rootfs / "lib/firmware/chip/controller.bin.xz"
        assert not (installed.with_suffix("").exists())
        assert (installed.stat().st_size) < (len(contents))
        assert (installed.stat().st_mode & 0o777) == (0o600)
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
        assert (decoder.decompress(installed.read_bytes())) == (contents)
        assert decoder.eof
        assert (decoder.check) == (lzma.CHECK_CRC32)
        firmware_inputs.verify_installed_firmware_inputs(rootfs, (firmware,))

        installed.write_bytes(b"invalid compressed firmware")
        with pytest.raises(SystemExit, match="installed firmware cannot be verified"):
            firmware_inputs.verify_installed_firmware_inputs(rootfs, (firmware,))
