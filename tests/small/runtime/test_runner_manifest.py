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


class RuntimeManifestTests:
    """The bundled runner accepts only its exact manifest contract."""

    def test_accepts_the_exact_runtime_contract(self, runtime_manifest_path: Path) -> None:
        """Accept a complete manifest with explicit USB interfaces."""
        loaded = load_manifest(runtime_manifest_path, runtime_manifest())

        assert (loaded["personalization"]["bytes"]) == (512)
        assert (loaded["usb"]["linux_gadget"]["keyboard_interface"]) == (1)
        assert (loaded["identity"]["target"]["display_name"]) == ("Demo Phone (D-1, D-2)")

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
