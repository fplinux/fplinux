# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter handoff behavior."""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport

from tests.small.runtime.adapter_fixtures import ADAPTER, BridgeFixture, BridgeProcess

if TYPE_CHECKING:
    from collections.abc import Callable


class HandoffTransportTests:
    """Select the post-bridge transport and report only a verified SSH session as ready."""

    @pytest.mark.parametrize(
        "matches",
        [pytest.param(False, id="other-kernel"), pytest.param(True, id="selected-kernel")],
    )
    def test_handoff_rejects_another_kernel_before_clock_sync_or_ready(
        self, *, matches: bool
    ) -> None:
        """An SSH session running another kernel is neither clock-synced nor reported ready."""

        def remote(
            _session: object,
            command: str,
            *,
            commands: list[str],
            matches: bool,
            **_options: object,
        ) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            if command == "uname -r":
                suffix = "9" * 16 if matches else "8" * 16
                stdout = f"6.18.42-fplinux-{suffix}\n"
            else:
                stdout = "system=1234 rtc=kept\n"
            return subprocess.CompletedProcess([], 0, stdout, "")

        session = {"session_id": "a" * 64}
        ready = {**session, "status": "ready"}
        commands: list[str] = []
        events: list[str] = []
        rendered = io.StringIO()

        def bound_session(
            _session: object, *, on_linux_usb: Callable[[], None], ready: object = ready
        ) -> object:
            on_linux_usb()
            return ready

        with (
            contextlib.redirect_stdout(rendered),
            mock.patch.dict(sys.modules, {"ssh_transport": ssh_transport}),
            mock.patch.object(ssh_transport, "wait_for_bound_session", side_effect=bound_session),
            mock.patch.object(
                ssh_transport,
                "run_remote",
                side_effect=partial(remote, commands=commands, matches=matches),
            ),
            mock.patch("fplinux_cli.runtime.ssh_transport.time.time", return_value=1234),
            contextlib.ExitStack() as stack,
        ):
            if not matches:
                stack.enter_context(pytest.raises(SystemExit, match="different kernel identity"))
            ADAPTER.complete_linux_handoff(
                {"transport": "usb-ncm"},
                session,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
                expected_device_identity="9" * 64,
                events=events.append,
            )
        assert (commands) == (["uname -r", "fplinux-clock 1234"] if matches else ["uname -r"])
        assert (events) == (["linux-usb", "ssh-ready"] if matches else ["linux-usb"])
        assert ("session is ready" in rendered.getvalue()) == (matches)

    @pytest.mark.parametrize(
        "failure",
        [
            pytest.param(None, id="authenticated"),
            pytest.param("authentication", id="authentication-failed"),
        ],
    )
    def test_ssh_ready_event_requires_an_authenticated_session(self, failure: str | None) -> None:
        """A failed SSH authentication never publishes a ready session to event consumers."""
        events: list[str] = []
        transport = mock.Mock()

        def bound_session(
            session: object,
            *,
            on_linux_usb: Callable[[], None],
            failure: str | None = failure,
        ) -> object:
            on_linux_usb()
            if failure == "authentication":
                raise SystemExit(failure)
            return session

        transport.wait_for_bound_session.side_effect = bound_session
        with (
            contextlib.redirect_stdout(io.StringIO()),
            mock.patch.object(ADAPTER.importlib, "import_module", return_value=transport),
            contextlib.ExitStack() as stack,
        ):
            if failure is not None:
                stack.enter_context(pytest.raises(SystemExit, match=failure))
            ADAPTER.complete_linux_handoff(
                {"transport": "usb-ncm"},
                {"session_id": "a" * 64},
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
                expected_device_identity="9" * 64,
                events=events.append,
            )
        assert (events) == (["linux-usb", "ssh-ready"] if failure is None else ["linux-usb"])

    def test_none_transport_returns_without_acquiring_ssh(self) -> None:
        """A no-transport profile completes after the bridge acknowledgement with no session."""
        session = {"session_id": "a" * 64}
        with (
            contextlib.redirect_stdout(io.StringIO()),
            mock.patch.object(
                ADAPTER.importlib,
                "import_module",
                side_effect=AssertionError("none transport must not acquire SSH/NCM"),
            ),
        ):
            result = ADAPTER.complete_linux_handoff(
                {"transport": "none"},
                session,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
                expected_device_identity="9" * 64,
            )

        assert (result) is None

    def test_usb_ncm_returns_the_authenticated_session(self) -> None:
        """The caller receives the session returned by the SSH transport, not the prepared one."""
        session = {"session_id": "a" * 64}
        transport = mock.Mock()
        ready = {"session_id": "a" * 64, "status": "ready"}
        transport.wait_for_bound_session.return_value = ready

        with (
            contextlib.redirect_stdout(io.StringIO()),
            mock.patch.object(ADAPTER.importlib, "import_module", return_value=transport),
        ):
            result = ADAPTER.complete_linux_handoff(
                {"transport": "usb-ncm"},
                session,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
                expected_device_identity="9" * 64,
            )

        assert (result) is (ready)


class BridgeAcknowledgementTests(BridgeFixture):
    """The loader result, bridge exit status and BootROM release gate the Linux transition."""

    def test_no_transport_events_end_at_acknowledged_linux_transition(self) -> None:
        """A bridge acknowledgement does not claim an authenticated SSH session."""
        events: list[str] = []
        self.run_bridge(BridgeProcess(0), events=events.append)
        assert (events) == (["waiting-for-device", "ram-loader-complete", "linux-transition"])

    def test_failed_bridge_never_emits_linux_transition(self) -> None:
        """A failed bridge cannot authorize the consumer's next transport stage."""
        events: list[str] = []
        with pytest.raises(SystemExit, match="did not acknowledge"):
            self.run_bridge(BridgeProcess(1), events=events.append)
        assert (events) == (["waiting-for-device", "ram-loader-complete"])

    def test_exit_zero_and_bootrom_disconnect_continue_with_the_exact_session_token(self) -> None:
        """Bridge success needs no output; the original USB node must then disappear."""
        bridge = BridgeProcess(0)
        popen, transport_module, bundle = self.run_bridge(bridge, bootrom_release_seconds=1.0)

        assert (popen.call_args.args[0]) == (
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
            ]
        )
        assert (popen.call_args.kwargs) == ({"cwd": bundle / "assets"})
        assert (bridge.wait_timeouts) == ([60])
        transport_module.remove_personalized_image.assert_called_once()

    def test_bridge_ack_timeout_fails_and_stops_the_bridge(self) -> None:
        """No bridge exit before the declared deadline is a failed handoff."""
        bridge = BridgeProcess(None, time_out=True)

        with pytest.raises(SystemExit, match="before the deadline"):
            self.run_bridge(bridge)

        assert bridge.terminated

    def test_exit_zero_without_bootrom_disconnect_fails(self) -> None:
        """A bridge acknowledgement alone cannot hide a stalled bootstrap transition."""
        bridge = BridgeProcess(0)

        with pytest.raises(SystemExit, match="BootROM USB did not disconnect"):
            self.run_bridge(bridge, bootrom_release_seconds=None)

    def test_none_transport_still_requires_a_prepared_session(self) -> None:
        """A host-only profile cannot bypass the per-run bridge binding token."""
        with pytest.raises(SystemExit, match="requires a prepared session"):
            ADAPTER.run(
                Path("bundle"), self.runtime("none"), None, expected_device_identity="9" * 64
            )
