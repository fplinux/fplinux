# SPDX-License-Identifier: GPL-2.0-only
"""Runtime-manifest format tests through the shipped RAM runner consumer."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any
from unittest import mock

import pytest
from fplinux_cli.runtime import bundle as bundle_runtime

from tests.fixtures.runtime_manifest import runtime_manifest

RUNNER = bundle_runtime


class BundleReadErrorTests:
    """Report an unreadable payload before any adapter is loaded."""

    def test_unreadable_payload_reports_the_path_without_exposing_an_os_exception(self) -> None:
        """Filesystem access denial is a normal command failure, including when run as root."""
        path = Path("image/ramboot.bin")
        # Replace only the OS read boundary; permission bits are ineffective under root.
        with (
            mock.patch.object(
                Path, "open", side_effect=PermissionError(13, "Permission denied", path)
            ),
            pytest.raises(SystemExit, match=r"fplinux run: .*Permission denied.*ramboot.bin"),
        ):
            RUNNER.digest(path)


class PythonRuntimeTests:
    """Keep the standalone runner on its single supported Python series."""

    def test_accepts_python_314(self) -> None:
        """Python 3.14 satisfies the standalone preflight."""
        with mock.patch.object(sys, "version_info", mock.Mock(major=3, minor=14)):
            RUNNER.host_preflight()

    @pytest.mark.parametrize(
        ("major", "minor"),
        [pytest.param(3, 13, id="python-3.13"), pytest.param(3, 15, id="python-3.15")],
    )
    def test_rejects_other_python_series(self, major: int, minor: int) -> None:
        """Older and unsupported newer interpreters fail before phone access."""
        with (
            mock.patch.object(
                sys,
                "version_info",
                mock.Mock(major=major, minor=minor),
            ),
            pytest.raises(
                SystemExit, match=rf"Python 3\.14 is required \(found {major}\.{minor}\)"
            ),
        ):
            RUNNER.host_preflight()


@pytest.fixture
def runtime_manifest_path(tmp_path: Path) -> Path:
    """Create one isolated runtime-manifest path."""
    return tmp_path / "runtime-manifest.json"


def load_manifest(runtime_manifest_path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    """Load one manifest through the shipped consumer."""
    runtime_manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    runtime = RUNNER.load_runtime_manifest(runtime_manifest_path)
    return RUNNER.validate_runtime_identity(runtime)


@pytest.fixture
def emmc_manifest() -> dict[str, Any]:
    """Declare optional radio data alongside mandatory audio for one MMC reader."""
    manifest = runtime_manifest()
    manifest["identity"]["platform"] = {
        "name": "sc9820e",
        "vendor": "Unisoc",
        "soc": "SC9820E",
        "aliases": [],
        "compatible": "sprd,sc9820e",
        "display_name": "Unisoc SC9820E",
    }
    manifest["emmc"] = {
        "raw_device": "/dev/mmcblk1",
        "files": [
            {"name": "audio_para", "locations": [["system", "/etc/audio_para"]], "max_bytes": 100}
        ],
        "acquisition_recipe": "a" * 64,
    }
    manifest["device_data"] = {
        "parser": None,
        "originals_parser": "targets/demo/phone_data.py",
        "ecc_module": None,
        "groups": {
            "audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}],
            "wcn-firmware": [{"source": "wcn.bin", "destination": "wcn.bin"}],
        },
        "required_groups": ["audio-profile"],
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        },
        "preparation_recipe": "b" * 64,
    }
    for relative in (
        "platforms/sc9820e/host/adapter.py",
        "targets/demo/phone_data.py",
        "scripts/fplinux_cli/device_data/emmc.py",
        "scripts/fplinux_cli/device_data/emmc-files/read_files.py",
        "scripts/fplinux_cli/runtime/emmc_files.py",
        "scripts/fplinux_cli/runtime/emmc-files/collect.sh",
        "scripts/ext4/__init__.py",
        "scripts/cachetools/__init__.py",
        "scripts/crcmod/__init__.py",
        "licenses/phone-data/ext4-LICENSE.txt",
        "licenses/phone-data/cachetools-LICENSE.txt",
        "licenses/phone-data/crcmod-LICENSE.txt",
    ):
        manifest["sha256"][relative] = "c" * 64
    return manifest


def test_emmc_manifest_retains_optional_groups_without_requiring_them(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any]
) -> None:
    loaded = load_manifest(runtime_manifest_path, emmc_manifest)
    assert loaded["emmc"]["raw_device"] == "/dev/mmcblk1"
    assert loaded["device_data"]["required_groups"] == ["audio-profile"]
    assert "wcn-firmware" in loaded["device_data"]["groups"]


@pytest.mark.parametrize("required", [[], ["unknown"], ["audio-profile", "audio-profile"], [1]])
def test_emmc_manifest_rejects_invalid_required_groups(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any], required: object
) -> None:
    emmc_manifest["device_data"]["required_groups"] = required
    with pytest.raises(SystemExit, match="required_groups must be"):
        load_manifest(runtime_manifest_path, emmc_manifest)


@pytest.mark.parametrize(
    "device", ["/dev/mmcblk1p1", "/dev/nand-raw", "/dev/mmcblk", "/outside/mmcblk1"]
)
def test_emmc_manifest_rejects_nonwhole_mmc_sources(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any], device: str
) -> None:
    emmc_manifest["emmc"]["raw_device"] = device
    with pytest.raises(SystemExit, match="whole /dev/mmcblkN"):
        load_manifest(runtime_manifest_path, emmc_manifest)


@pytest.mark.parametrize(
    "missing",
    [
        "targets/demo/phone_data.py",
        "scripts/fplinux_cli/runtime/emmc-files/collect.sh",
        "scripts/fplinux_cli/device_data/emmc-files/read_files.py",
        "scripts/ext4/__init__.py",
        "licenses/phone-data/crcmod-LICENSE.txt",
    ],
)
def test_emmc_manifest_rejects_an_incomplete_preparation_closure(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any], missing: str
) -> None:
    del emmc_manifest["sha256"][missing]
    with pytest.raises(SystemExit, match="required runtime closure"):
        load_manifest(runtime_manifest_path, emmc_manifest)


def test_emmc_manifest_uses_the_shared_original_location_admission(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any]
) -> None:
    emmc_manifest["emmc"]["files"][0]["locations"] = [["system", "/etc/../private"]]
    with pytest.raises(SystemExit, match="unsafe source location"):
        load_manifest(runtime_manifest_path, emmc_manifest)


def test_emmc_manifest_rejects_a_reader_for_another_platform(
    runtime_manifest_path: Path, emmc_manifest: dict[str, Any]
) -> None:
    emmc_manifest["identity"]["platform"]["name"] = "ums9117"
    with pytest.raises(SystemExit, match="does not support the runtime eMMC reader"):
        load_manifest(runtime_manifest_path, emmc_manifest)


class RuntimeManifestTests:
    """The bundled runner accepts only its exact manifest contract."""

    @pytest.mark.parametrize("recipe", [None, "", "a" * 63, "A" * 64, "g" * 64])
    def test_rejects_nand_without_a_content_derived_acquisition_recipe(
        self, runtime_manifest_path: Path, recipe: object
    ) -> None:
        """A runner cannot reuse a raw backup using an absent or malformed identity."""
        manifest = runtime_manifest()
        manifest["nand"] = {"raw_device": "/dev/raw-nand", "acquisition_recipe": recipe}
        with pytest.raises(SystemExit, match="acquisition_recipe must be"):
            load_manifest(runtime_manifest_path, manifest)

    def test_accepts_nand_identity_without_preparation_groups(
        self, runtime_manifest_path: Path
    ) -> None:
        """Raw backup support does not require a derived phone-data parser."""
        manifest = runtime_manifest()
        manifest["nand"] = {"raw_device": "/dev/raw-nand", "acquisition_recipe": "a" * 64}
        loaded = load_manifest(runtime_manifest_path, manifest)
        assert loaded["nand"]["acquisition_recipe"] == "a" * 64
        assert loaded["device_data"] is None

    def test_accepts_the_exact_runtime_contract(self, runtime_manifest_path: Path) -> None:
        """Accept a complete manifest with explicit USB interfaces."""
        loaded = load_manifest(runtime_manifest_path, runtime_manifest())

        assert (loaded["personalization"]["bytes"]) == (512)
        assert (loaded["usb"]["linux_gadget"]["keyboard_interface"]) == (1)
        assert (loaded["identity"]["target"]["display_name"]) == ("Demo Phone (D-1, D-2)")
        assert loaded["release_id"] == "aaaaaaaaaaaa"
        assert loaded["source_commit"] == "a" * 40
        assert loaded["source_snapshot"] == "b" * 64

    def test_accepts_a_source_archive_without_a_git_commit(
        self, runtime_manifest_path: Path
    ) -> None:
        """A source snapshot remains explicit when its archive has no Git checkout."""
        manifest = runtime_manifest()
        manifest["release_id"] = "bbbbbbbbbbbb"
        manifest["source_commit"] = None

        loaded = load_manifest(runtime_manifest_path, manifest)

        assert loaded["source_commit"] is None

    @pytest.mark.parametrize("field", ["release_id", "source_commit", "source_snapshot"])
    def test_rejects_a_runtime_without_its_complete_release_identity(
        self, runtime_manifest_path: Path, field: str
    ) -> None:
        """Every release field is mandatory, including a nullable source commit."""
        manifest = runtime_manifest()
        del manifest[field]

        with pytest.raises(SystemExit, match="runtime manifest must contain exactly"):
            load_manifest(runtime_manifest_path, manifest)

    @pytest.mark.parametrize("field", ["release_id", "source_commit", "source_snapshot"])
    @pytest.mark.parametrize("value", ["", 1])
    def test_rejects_an_empty_or_nonstring_release_identity(
        self, runtime_manifest_path: Path, field: str, value: object
    ) -> None:
        """Present release fields cannot replace their source identity with empty data."""
        manifest = runtime_manifest()
        manifest[field] = value

        with pytest.raises(SystemExit, match="must be a non-empty string"):
            load_manifest(runtime_manifest_path, manifest)

    @pytest.mark.parametrize(
        ("build_type", "accepted"),
        [
            pytest.param("release", True, id="release"),
            pytest.param("debug", True, id="debug"),
            pytest.param(None, False, id="missing"),
            pytest.param("other", False, id="unknown"),
        ],
    )
    def test_build_type_is_explicit_and_has_no_missing_or_unknown_fallback(
        self, runtime_manifest_path: Path, build_type: str | None, *, accepted: bool
    ) -> None:
        """A standalone runner cannot reinterpret a bundle's kernel capabilities."""
        manifest = runtime_manifest()
        if build_type is None:
            del manifest["build_type"]
        else:
            manifest["build_type"] = build_type
        if accepted:
            assert (load_manifest(runtime_manifest_path, manifest)["build_type"]) == (build_type)
        else:
            with pytest.raises(SystemExit):
                load_manifest(runtime_manifest_path, manifest)

    def test_manifest_validation_does_not_execute_identity_code(
        self, runtime_manifest_path: Path
    ) -> None:
        """Manifest validation returns identity data without importing its implementation."""
        runtime_manifest_path.write_text(json.dumps(runtime_manifest()), encoding="utf-8")
        with mock.patch.object(
            RUNNER,
            "identity_module",
            side_effect=AssertionError("identity helper executed too early"),
        ):
            loaded = RUNNER.load_runtime_manifest(runtime_manifest_path)

        assert isinstance(loaded["identity"], dict)

    def test_rejects_a_handoff_without_authenticated_usb_transport(
        self, runtime_manifest_path: Path
    ) -> None:
        """Every delivered phone runtime requires its authenticated USB connection."""
        manifest = runtime_manifest()
        manifest["transport"] = "none"

        with pytest.raises(SystemExit, match="runtime transport"):
            load_manifest(runtime_manifest_path, manifest)

    def test_accepts_identity_without_unverified_codes_or_aliases(
        self, runtime_manifest_path: Path
    ) -> None:
        """An empty token array remains an explicit statement that no code is known."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["hardware_codes"] = []
        manifest["identity"]["target"]["display_name"] = "Demo Phone"
        manifest["identity"]["platform"]["aliases"] = []

        loaded = load_manifest(runtime_manifest_path, manifest)

        assert (loaded["identity"]["target"]["hardware_codes"]) == ([])
        assert (loaded["identity"]["platform"]["aliases"]) == ([])

    def test_rejects_one_compatible_for_both_target_and_platform(
        self, runtime_manifest_path: Path
    ) -> None:
        """Require an exact machine identity followed by a distinct SoC fallback."""
        manifest = runtime_manifest()
        manifest["identity"]["platform"]["compatible"] = "demo,phone"

        with pytest.raises(SystemExit, match="compatibles must differ"):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_an_unknown_host_transport(self, runtime_manifest_path: Path) -> None:
        """Transport selection remains a closed runtime contract."""
        manifest = runtime_manifest()
        manifest["transport"] = "serial"

        with pytest.raises(SystemExit, match="runtime transport must be usb-ncm"):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_a_runtime_manifest_missing_required_fields(
        self, runtime_manifest_path: Path
    ) -> None:
        """Omitted profile and transport fields are rejected rather than defaulted."""
        manifest = runtime_manifest()
        del manifest["profile"]
        del manifest["transport"]

        with pytest.raises(SystemExit, match="runtime manifest must contain exactly"):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_an_unknown_runtime_field(self, runtime_manifest_path: Path) -> None:
        """Reject fields outside the exact runtime contract."""
        manifest = runtime_manifest()
        manifest["unexpected"] = "value"

        with pytest.raises(SystemExit, match="runtime manifest must contain exactly"):
            load_manifest(runtime_manifest_path, manifest)

    @pytest.mark.parametrize(
        "brand",
        [
            pytest.param(" Demo", id="leading-space"),
            pytest.param("Demo  Devices", id="double-space"),
            pytest.param("Démo", id="non-ascii"),
            pytest.param("Demo\nDevices", id="newline"),
        ],
    )
    def test_rejects_noncanonical_identity_text(
        self, runtime_manifest_path: Path, brand: str
    ) -> None:
        """Identity text is stable printable ASCII without padding or doubled spaces."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["brand"] = brand
        with (
            pytest.raises(SystemExit, match="canonical printable ASCII text"),
        ):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_identity_that_does_not_derive_its_display_name(
        self, runtime_manifest_path: Path
    ) -> None:
        """Display names cannot become a second independently editable identity."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["display_name"] = "Independent label"

        with pytest.raises(SystemExit, match="display_name must be derived"):
            load_manifest(runtime_manifest_path, manifest)

        manifest = runtime_manifest()
        manifest["identity"]["platform"]["display_name"] = "Independent platform"

        with pytest.raises(SystemExit, match="display_name must be derived"):
            load_manifest(runtime_manifest_path, manifest)

    @pytest.mark.parametrize(
        "hardware_codes",
        [
            pytest.param("D-1", id="scalar"),
            pytest.param(["lowercase"], id="lowercase"),
            pytest.param(["D-1", "D-1"], id="duplicate"),
        ],
    )
    def test_rejects_invalid_or_duplicate_hardware_tokens(
        self, runtime_manifest_path: Path, hardware_codes: object
    ) -> None:
        """Hardware codes and aliases remain ordered arrays of unique tokens."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["hardware_codes"] = hardware_codes
        with pytest.raises(SystemExit):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_alias_equal_to_the_platform_soc(self, runtime_manifest_path: Path) -> None:
        """An alias must add identity instead of repeating the canonical SoC name."""
        manifest = runtime_manifest()
        manifest["identity"]["platform"]["aliases"] = ["UMS9117"]

        with pytest.raises(SystemExit, match="aliases must not repeat the SoC name"):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_noncanonical_compatible(self, runtime_manifest_path: Path) -> None:
        """Runtime identity uses one lowercase vendor,device compatible."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["compatible"] = "Nokia,TA-1618"

        with pytest.raises(SystemExit, match="lowercase vendor,device compatible"):
            load_manifest(runtime_manifest_path, manifest)

    def test_rejects_runtime_without_declared_keyboard_interface(
        self, runtime_manifest_path: Path
    ) -> None:
        """Require every current Linux USB interface explicitly."""
        manifest = runtime_manifest()
        del manifest["usb"]["linux_gadget"]["keyboard_interface"]

        with pytest.raises(SystemExit, match="linux_gadget USB must contain exactly"):
            load_manifest(runtime_manifest_path, manifest)

    @pytest.mark.parametrize(
        "helper",
        [
            pytest.param("scripts/fplinux_cli/runtime/ssh_transport.py", id="ssh-transport"),
            pytest.param("scripts/fplinux_cli/manifests/identity.py", id="identity"),
        ],
    )
    def test_rejects_a_runtime_without_a_mandatory_runner_helper_hash(
        self, runtime_manifest_path: Path, helper: str
    ) -> None:
        """Bind both the SSH and identity consumers into the runtime closure."""
        manifest = runtime_manifest()
        del manifest["sha256"][helper]

        with (
            pytest.raises(SystemExit, match="runtime hashes do not include"),
        ):
            load_manifest(runtime_manifest_path, manifest)
