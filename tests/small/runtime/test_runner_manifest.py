# SPDX-License-Identifier: GPL-2.0-only
"""Runtime-manifest format tests through the shipped RAM runner consumer."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast
from unittest import mock

import pytest

from common import loader_events
from tests import ROOT
from tests.fixtures.runtime_manifest import runtime_manifest

if TYPE_CHECKING:
    from types import ModuleType


def load_runner() -> ModuleType:
    """Load the standalone runner as a normal Python module."""
    path = ROOT / "common/run.py"
    spec = importlib.util.spec_from_file_location("fplinux_ram_runner", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load RAM runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_runner()


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
        with mock.patch.object(RUNNER.sys, "version_info", mock.Mock(major=3, minor=14)):
            RUNNER.host_preflight()

    @pytest.mark.parametrize(
        ("major", "minor"),
        [pytest.param(3, 13, id="python-3.13"), pytest.param(3, 15, id="python-3.15")],
    )
    def test_rejects_other_python_series(self, major: int, minor: int) -> None:
        """Older and unsupported newer interpreters fail before phone access."""
        with (
            mock.patch.object(
                RUNNER.sys,
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
    runtime = cast("dict[str, Any]", RUNNER.load_runtime_manifest(runtime_manifest_path))
    return cast("dict[str, Any]", RUNNER.validate_runtime_identity(runtime))


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

    def test_accepts_an_explicit_no_transport_handoff_contract(
        self, runtime_manifest_path: Path
    ) -> None:
        """A host-only bundle declares that it will not create USB-NCM transport."""
        manifest = runtime_manifest()
        manifest["transport"] = "none"

        loaded = load_manifest(runtime_manifest_path, manifest)

        assert (loaded["transport"]) == ("none")

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

        with pytest.raises(SystemExit, match="runtime transport must be one of"):
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
            pytest.param("runner/ssh_transport.py", id="ssh-transport"),
            pytest.param("runner/identity.py", id="identity"),
        ],
    )
    def test_rejects_a_runtime_without_a_mandatory_runner_helper_hash(
        self, runtime_manifest_path: Path, helper: str
    ) -> None:
        """Bind both the SSH and identity consumers into the runtime closure."""
        manifest = runtime_manifest()
        del manifest["sha256"][helper]

        with (
            pytest.raises(SystemExit, match="runtime hashes must contain exactly"),
        ):
            load_manifest(runtime_manifest_path, manifest)


@dataclass(frozen=True)
class RunnerBundle:
    """A regular bundle and the runner path used by its entry point."""

    bundle: Path
    runner: Path


@pytest.fixture
def no_transport_bundle(tmp_path: Path) -> RunnerBundle:
    """Build one regular hashed bundle that has no USB-NCM transport."""
    bundle = tmp_path / "bundle"
    runner = bundle / "runner/run.py"
    manifest = runtime_manifest()
    manifest["transport"] = "none"
    payloads = {
        "image/ramboot.bin": b"DHTB RAM image\n",
        "runner/identity.py": (ROOT / "scripts/fplinux_cli/manifests/identity.py").read_bytes(),
        "runner/platform_adapter.py": b"adapter\n",
        "runner/ssh_transport.py": b"unused but hashed helper\n",
        "runner/loader_events.py": (ROOT / "common/loader_events.py").read_bytes(),
        "assets/fdl1.bin": b"fdl1\n",
        "host/loader": b"loader\n",
    }
    for relative, data in payloads.items():
        path = bundle / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755 if relative == "host/loader" else 0o644)
        manifest["sha256"][relative] = hashlib.sha256(data).hexdigest()
    runner.parent.mkdir(parents=True, exist_ok=True)
    runner.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
    runner.chmod(0o755)
    (bundle / "runtime-manifest.json").write_text(
        json.dumps(manifest),
        encoding="utf-8",
    )

    return RunnerBundle(bundle=bundle, runner=runner)


class NoTransportRunnerTests:
    """Exercise the shipped runner's no-transport handoff boundary."""

    def test_none_transport_hands_session_to_adapter_without_ssh_handoff(
        self, no_transport_bundle: RunnerBundle
    ) -> None:
        """A host-only run passes its session to the adapter and finishes it without a shell."""
        adapter = mock.Mock()
        adapter.run.return_value = None
        session = {"image": str(no_transport_bundle.bundle / "image/ramboot.bin")}
        ssh = mock.Mock()
        ssh.build_manifest_device_identity.return_value = "e" * 64
        ssh.prepare_session.return_value = session
        with (
            mock.patch.object(RUNNER, "__file__", str(no_transport_bundle.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(RUNNER, "load_adapter", return_value=adapter),
            mock.patch.object(
                RUNNER,
                "load_module",
                side_effect=lambda _path, name: (
                    loader_events if name == "fplinux_loader_events" else ssh
                ),
            ),
            mock.patch.object(sys, "argv", [str(no_transport_bundle.runner)]),
        ):
            RUNNER.main()

        runtime = adapter.run.call_args.args[1]
        assert (runtime["transport"]) == ("none")
        assert (adapter.run.call_args.args[2]) is (session)
        assert (adapter.run.call_args.kwargs["expected_device_identity"]) == ("e" * 64)
        ssh.prepare_session.assert_called_once()
        ssh.wait_for_bound_session.assert_not_called()
        ssh.open_shell.assert_not_called()
        ssh.finish_session.assert_called_once_with(session)

    def test_standalone_reconnect_checks_the_running_device_identity(
        self, no_transport_bundle: RunnerBundle
    ) -> None:
        """Reconnect through the bundle manifest's kernel identity, not its generation."""
        runtime_path = no_transport_bundle.bundle / "runtime-manifest.json"
        manifest = json.loads(runtime_path.read_text(encoding="utf-8"))
        manifest["transport"] = "usb-ncm"
        runtime_path.write_text(json.dumps(manifest), encoding="utf-8")
        session: dict[str, str] = {}
        ready = {**session, "interface": "usb0"}
        result = mock.Mock(returncode=0)
        ssh = mock.Mock()
        ssh.build_manifest_device_identity.return_value = "e" * 64
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = ready
        ssh.run_remote.return_value = result

        with (
            mock.patch.object(RUNNER, "__file__", str(no_transport_bundle.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(
                RUNNER,
                "load_module",
                side_effect=lambda _path, name: (
                    loader_events if name == "fplinux_loader_events" else ssh
                ),
            ),
            mock.patch.object(
                sys,
                "argv",
                [str(no_transport_bundle.runner), "--reconnect", "--exec", "true"],
            ),
        ):
            RUNNER.main()

        ssh.require_device_identity.assert_called_once_with(ready, "e" * 64)
        ssh.run_remote.assert_called_once_with(ready, "true")
        # Only a fresh run sets the phone clock; reconnecting leaves it running.
        ssh.sync_clock.assert_not_called()

    def test_standalone_reconnect_rejects_another_device_before_remote_action(
        self, no_transport_bundle: RunnerBundle
    ) -> None:
        """A mismatched kernel identity prevents the requested standalone command."""
        runtime_path = no_transport_bundle.bundle / "runtime-manifest.json"
        manifest = json.loads(runtime_path.read_text(encoding="utf-8"))
        manifest["transport"] = "usb-ncm"
        runtime_path.write_text(json.dumps(manifest), encoding="utf-8")
        session: dict[str, str] = {}
        ready = {**session, "interface": "usb0"}
        ssh = mock.Mock()
        ssh.build_manifest_device_identity.return_value = "e" * 64
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = ready
        ssh.require_device_identity.side_effect = SystemExit(
            "fplinux ssh: current SSH session exposes a different kernel identity"
        )

        with (
            mock.patch.object(RUNNER, "__file__", str(no_transport_bundle.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(
                RUNNER,
                "load_module",
                side_effect=lambda _path, name: (
                    loader_events if name == "fplinux_loader_events" else ssh
                ),
            ),
            mock.patch.object(
                sys,
                "argv",
                [
                    str(no_transport_bundle.runner),
                    "--reconnect",
                    "--exec",
                    "touch /tmp/should-not-run",
                ],
            ),
            pytest.raises(SystemExit, match="different kernel identity"),
        ):
            RUNNER.main()

        ssh.require_device_identity.assert_called_once_with(ready, "e" * 64)
        ssh.run_remote.assert_not_called()

    def test_standalone_reconnect_rejects_unverified_bundle_contents_first(
        self, no_transport_bundle: RunnerBundle
    ) -> None:
        """A bundle that fails its content check never reaches the phone session."""
        runtime_path = no_transport_bundle.bundle / "runtime-manifest.json"
        manifest = json.loads(runtime_path.read_text(encoding="utf-8"))
        manifest["transport"] = "usb-ncm"
        runtime_path.write_text(json.dumps(manifest), encoding="utf-8")
        ssh = mock.Mock()
        ssh.bundle_identity.side_effect = SystemExit("fplinux: runtime image changed")

        with (
            mock.patch.object(RUNNER, "__file__", str(no_transport_bundle.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(
                RUNNER,
                "load_module",
                side_effect=lambda _path, name: (
                    loader_events if name == "fplinux_loader_events" else ssh
                ),
            ),
            mock.patch.object(
                sys,
                "argv",
                [
                    str(no_transport_bundle.runner),
                    "--reconnect",
                    "--exec",
                    "touch /tmp/should-not-run",
                ],
            ),
            pytest.raises(SystemExit, match="runtime image changed"),
        ):
            RUNNER.main()

        ssh.load_current_session.assert_not_called()
        ssh.run_remote.assert_not_called()
