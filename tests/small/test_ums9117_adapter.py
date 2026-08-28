# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the data-driven UMS9117 RAM adapter."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]


def load_adapter() -> ModuleType:
    """Load the shipped standalone adapter without requiring a Python package."""
    path = ROOT / "platforms/ums9117/host/adapter.py"
    spec = importlib.util.spec_from_file_location("ums9117_host_adapter", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load adapter: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ADAPTER = load_adapter()


class UsbDeviceAccessTests(unittest.TestCase):
    """Report disappearance and permission denial as different USB failures."""

    device = Path("/dev/bus/usb/007/053")

    def test_vanished_node_reports_disconnect(self) -> None:
        """A transient USB disappearance must not be diagnosed as a udev failure."""
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=FileNotFoundError),
            self.assertRaisesRegex(SystemExit, "disconnected before the RAM loader"),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_permission_denial_reports_access_control(self) -> None:
        """An existing node rejected by the OS keeps the actionable udev diagnosis."""
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=PermissionError),
            self.assertRaisesRegex(SystemExit, "not readable and writable"),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_successful_probe_closes_the_usbfs_node(self) -> None:
        """The access probe must release its descriptor before libc opens the device."""
        with (
            mock.patch.object(ADAPTER.os, "open", return_value=41) as open_device,
            mock.patch.object(ADAPTER.os, "close") as close_device,
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

        open_device.assert_called_once_with(
            self.device,
            ADAPTER.os.O_RDWR | ADAPTER.os.O_CLOEXEC,
        )
        close_device.assert_called_once_with(41)


def adapter_data(**overrides: object) -> dict[str, object]:
    """Return one valid adapter table with optional focused overrides."""
    data: dict[str, object] = {
        "brightness": 50,
        "rotation": 0,
        "spi_mode": 1,
        "lcd_id": 0x8888B6,
        "exec_distance": 0x314D,
        "backlight_channels": "rgbw",
        "backlight_level": 0x14,
        "session_name": "test-linux",
        "handoff_wait_seconds": 60,
        "usb_release_wait_seconds": 10,
        "boot_instructions": "Hold the boot key and connect USB.",
    }
    data.update(overrides)
    return data


def runtime_identity(*, platform_name: str = "ums9117") -> dict[str, object]:
    """Return the identity portion consumed by the fixed UMS9117 adapter."""
    return {
        "identity": {
            "target": {"display_name": "Nokia 3210 4G (TA-1618)"},
            "platform": {"name": platform_name},
        }
    }


class RuntimeIdentityTests(unittest.TestCase):
    """Keep platform selection and the rendered target identity fail-closed."""

    def test_reads_the_nested_target_display_name(self) -> None:
        """The adapter renders the validated target identity rather than a root alias."""
        runtime = runtime_identity()
        runtime["display_name"] = "obsolete root name"

        self.assertEqual(
            ADAPTER.runtime_target_display_name(runtime),
            "Nokia 3210 4G (TA-1618)",
        )

    def test_rejects_a_bundle_for_another_platform(self) -> None:
        """The fixed adapter refuses a runtime manifest for another platform."""
        with self.assertRaisesRegex(SystemExit, "platform identity must name ums9117"):
            ADAPTER.runtime_target_display_name(runtime_identity(platform_name="other"))

    def test_rejects_the_legacy_root_only_identity(self) -> None:
        """A previous root display_name/platform pair cannot select this adapter."""
        with self.assertRaisesRegex(SystemExit, "runtime identity must be an object"):
            ADAPTER.runtime_target_display_name(
                {"display_name": "Demo Phone", "platform": "ums9117"}
            )


class LoaderArgumentsTests(unittest.TestCase):
    """Keep exec-distance selection entirely target-driven."""

    def test_nonzero_exec_distance_precedes_ram_loads(self) -> None:
        """Emit the optional setup command before both RAM FDL commands."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0x314D,
        )
        self.assertEqual(
            arguments,
            [
                "loader",
                "t117_exec_dist",
                "0x314d",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ],
        )

    def test_zero_exec_distance_omits_setup_command(self) -> None:
        """Start directly with the RAM FDL load when the target requests zero."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0,
        )
        self.assertEqual(
            arguments,
            [
                "loader",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ],
        )


class BacklightConfigurationTests(unittest.TestCase):
    """Reject ambiguous channel spelling and out-of-range levels."""

    def test_canonical_channel_subset_formats_bl_extra(self) -> None:
        """Preserve the selected RGB subset in the bridge argument."""
        config = ADAPTER.adapter_config(
            adapter_data(backlight_channels="rgb", backlight_level=0x1F)
        )
        self.assertEqual(ADAPTER.backlight_argument(config), "rgb=0x1f")

    def test_noncanonical_or_invalid_channels_are_rejected(self) -> None:
        """Accept only non-empty channels in canonical rgbw order."""
        for channels in ("", "gr", "rr", "rgbwx"):
            with (
                self.subTest(channels=channels),
                self.assertRaisesRegex(
                    SystemExit,
                    "backlight_channels",
                ),
            ):
                ADAPTER.adapter_config(adapter_data(backlight_channels=channels))

    def test_backlight_level_above_encoded_range_is_rejected(self) -> None:
        """Keep the level within the six-bit libc_server field."""
        with self.assertRaisesRegex(SystemExit, "backlight_level"):
            ADAPTER.adapter_config(adapter_data(backlight_level=0x40))


class HandoffTransportTests(unittest.TestCase):
    """Keep post-bridge transport selection independent of bridge diagnostics."""

    def test_none_transport_returns_without_acquiring_ssh(self) -> None:
        """A no-transport profile completes after the bridge acknowledgement."""
        rendered = io.StringIO()
        session = {"session_id": "a" * 64}
        with (
            contextlib.redirect_stdout(rendered),
            mock.patch.object(
                ADAPTER.importlib,
                "import_module",
                side_effect=AssertionError("none transport must not acquire SSH/NCM"),
            ),
        ):
            ADAPTER.complete_linux_handoff(
                {"transport": "none"},
                session,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
            )

        self.assertEqual(
            rendered.getvalue(),
            "Bridge acknowledged the Linux transition; no host-side transport is selected.\n",
        )

    def test_usb_ncm_waits_for_the_prepared_session(self) -> None:
        """The USB-NCM path acquires exactly the session that bridge acknowledged."""
        session = {"session_id": "a" * 64}
        transport = mock.Mock()
        ready = {"session_id": "a" * 64, "status": "ready"}
        transport.wait_for_bound_session.return_value = ready

        with (
            contextlib.redirect_stdout(io.StringIO()),
            mock.patch.object(ADAPTER.importlib, "import_module", return_value=transport),
            self.assertRaisesRegex(SystemExit, "SSH client returned"),
        ):
            ADAPTER.complete_linux_handoff(
                {"transport": "usb-ncm"},
                session,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
            )

        transport.wait_for_bound_session.assert_called_once_with(session)
        transport.open_shell.assert_called_once_with(ready)


class BridgeProcess:
    """Minimal bridge process whose exit code is the protocol result."""

    def __init__(
        self,
        status: int | None,
        *,
        time_out: bool = False,
        diagnostics: str = "",
    ) -> None:
        """Set one exit outcome and optional human-readable diagnostics."""
        self.status = status
        self.time_out = time_out
        self.diagnostics = diagnostics
        self.returncode: int | None = None
        self.wait_timeouts: list[int] = []
        self.terminated = False
        self.killed = False

    def poll(self) -> int | None:
        """Expose the process state used by the adapter cleanup path."""
        return self.returncode

    def wait(self, timeout: int | None = None) -> int:
        """Report one configured bridge outcome, then allow deterministic cleanup."""
        if timeout is not None:
            self.wait_timeouts.append(timeout)
        if self.time_out:
            self.time_out = False
            command = "libc_server"
            wait_timeout = timeout if timeout is not None else 0
            raise subprocess.TimeoutExpired(command, wait_timeout)
        if self.returncode is None:
            self.returncode = self.status
        if self.returncode is None:
            message = "bridge cleanup did not choose an exit status"
            raise AssertionError(message)
        return self.returncode

    def terminate(self) -> None:
        """Model normal process termination after an acknowledgement timeout."""
        self.terminated = True
        self.status = -15

    def kill(self) -> None:
        """Model forced process termination if normal termination did not complete."""
        self.killed = True
        self.status = -9


class BridgeAcknowledgementTests(unittest.TestCase):
    """The bridge exit status, not its diagnostics, authorizes Linux startup."""

    session_id = "a" * 64

    def runtime(
        self,
        transport: str = "none",
    ) -> dict[str, Any]:
        """Return the complete adapter input for one bridge acknowledgement case."""
        return {
            **runtime_identity(),
            "transport": transport,
            "assets": {
                "fdl1": "assets/fdl1.bin",
                "pinmap": "assets/pinmap.bin",
                "keymap": "assets/keymap.bin",
            },
            "host_tools": {
                "loader": "host/spd_dump",
                "bridge": "host/libc_server",
                "keyboard": "host/fplinux-usb-keyboard",
            },
            "adapter": adapter_data(),
            "image": "image/ramboot.bin",
            "addresses": {"fdl1": 0x6200, "payload": 0x80100000},
            "usb": {
                "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 1},
                "linux_gadget": {
                    "vendor_id": 0x0525,
                    "product_id": 0xA4A6,
                    "wait_seconds": 30,
                },
            },
        }

    def session(self) -> dict[str, str]:
        """Return the prepared session selected by the current runner invocation."""
        return {"session_id": self.session_id, "image": str(ROOT / "ramboot.bin")}

    def run_bridge(
        self,
        bridge: BridgeProcess,
        *,
        transport: str = "none",
        bootrom_states: list[bool] | None = None,
        monotonic_values: list[int] | None = None,
    ) -> tuple[mock.Mock, mock.Mock, Path, mock.Mock]:
        """Run the adapter through its bridge-process boundary with no phone attached."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            bootrom = mock.Mock(spec=Path)
            bootrom.exists.side_effect = bootrom_states or [False]
            transport_module = mock.Mock()
            popen = mock.Mock(return_value=bridge)
            patches = [
                mock.patch.object(ADAPTER, "usb_device_path", return_value=bootrom),
                mock.patch.object(ADAPTER, "require_usb_device_access"),
                mock.patch.object(ADAPTER.time, "sleep"),
                mock.patch.object(
                    ADAPTER.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ),
                mock.patch.object(ADAPTER.subprocess, "Popen", popen),
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
            ]
            if monotonic_values is not None:
                patches.append(
                    mock.patch.object(ADAPTER.time, "monotonic", side_effect=monotonic_values)
                )
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                for patch in patches:
                    stack.enter_context(patch)
                ADAPTER.run(
                    bundle,
                    self.runtime(transport),
                    self.session(),
                )
        return popen, transport_module, bundle, bootrom

    def test_adapter_preserves_runner_owned_signal_handlers(self) -> None:
        """The adapter must not replace the outer runner's process-lifecycle owner."""
        signals = (signal.SIGINT, signal.SIGTERM)
        before = {signum: signal.getsignal(signum) for signum in signals}
        try:
            self.run_bridge(BridgeProcess(0), bootrom_states=[True, False])
            self.assertEqual(
                {signum: signal.getsignal(signum) for signum in signals},
                before,
            )
        finally:
            for signum, handler in before.items():
                signal.signal(signum, handler)

    def test_exit_zero_and_bootrom_disconnect_continue_with_the_exact_session_token(self) -> None:
        """Bridge success needs no output; the original USB node must then disappear."""
        bridge = BridgeProcess(0, diagnostics="arbitrary diagnostic output")
        popen, transport_module, bundle, bootrom = self.run_bridge(
            bridge,
            bootrom_states=[True, False],
        )

        self.assertEqual(
            popen.call_args.args[0],
            [
                "/usr/bin/stdbuf",
                "-oL",
                "-eL",
                str(bundle / "host/libc_server"),
                "--fplinux-handoff",
                self.session_id,
                "--",
                "--bright",
                "50",
                "--rotate",
                "0",
                "--spi_mode",
                "1",
                "--lcd",
                "0x8888b6",
                "--bl_extra",
                "rgbw=0x14",
                "test-linux",
            ],
        )
        self.assertEqual(popen.call_args.kwargs, {"cwd": bundle / "assets"})
        self.assertNotIn("stdout", popen.call_args.kwargs)
        self.assertNotIn("stderr", popen.call_args.kwargs)
        self.assertEqual(bridge.wait_timeouts, [60])
        self.assertEqual(bootrom.exists.call_count, 2)
        transport_module.remove_personalized_image.assert_called_once()

    def test_marker_like_text_cannot_replace_a_nonzero_bridge_ack(self) -> None:
        """A bridge diagnostic cannot authorize Linux when the bridge exits unsuccessfully."""
        diagnostics = "TA1618_LINUX_BOOTSTRAP stage=5 message=PREPARE LINUX"
        bridge = BridgeProcess(19, diagnostics=diagnostics)

        with self.assertRaisesRegex(SystemExit, "status 19"):
            self.run_bridge(bridge)

    def test_bridge_ack_timeout_fails_and_stops_the_bridge(self) -> None:
        """No bridge exit before the declared deadline is a failed handoff."""
        bridge = BridgeProcess(None, time_out=True)

        with self.assertRaisesRegex(SystemExit, "before the deadline"):
            self.run_bridge(bridge)

        self.assertTrue(bridge.terminated)

    def test_exit_zero_without_bootrom_disconnect_fails(self) -> None:
        """A bridge acknowledgement alone cannot hide a stalled bootstrap transition."""
        bridge = BridgeProcess(0)

        with self.assertRaisesRegex(SystemExit, "BootROM USB did not disconnect"):
            self.run_bridge(
                bridge,
                bootrom_states=[True],
                monotonic_values=[0, 0, 10],
            )

    def test_none_transport_still_requires_a_prepared_session(self) -> None:
        """A host-only profile cannot bypass the per-run bridge binding token."""
        with self.assertRaisesRegex(SystemExit, "requires a prepared session"):
            ADAPTER.run(Path("bundle"), self.runtime("none"), None)


if __name__ == "__main__":
    unittest.main()
