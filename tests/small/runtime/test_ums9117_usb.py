# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter USB access and desktop probe exclusion."""

from __future__ import annotations

from pathlib import Path
from unittest import mock

import pytest

from tests import ROOT
from tests.small.runtime.adapter_fixtures import ADAPTER, FakeClock


class UdevRulesTests:
    """Keep desktop device discovery away from the RAM transport."""

    def test_usb_rule_precedes_libmtp_and_suppresses_its_probe(self) -> None:
        """BootROM and Linux gadget events must not run the generic MTP probe."""
        rules = ROOT / "common/60-fplinux.rules"
        assert (int(rules.name.partition("-")[0])) < (69)
        lines = [
            line
            for line in rules.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]

        for vendor, product in (("1782", "4d00"), ("0525", "a4a6")):
            matches = [
                line
                for line in lines
                if f'ATTR{{idVendor}}=="{vendor}"' in line
                and f'ATTR{{idProduct}}=="{product}"' in line
            ]
            assert (len(matches)) == (1)
            assert ('ENV{MTP_NO_PROBE}="1"') in (matches[0])


class UsbDeviceAccessTests:
    """Report disappearance and permission denial as different USB failures."""

    device = Path("/dev/bus/usb/007/053")

    def test_vanished_node_reports_disconnect(self) -> None:
        """A transient USB disappearance must not be diagnosed as a udev failure."""
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=FileNotFoundError),
            pytest.raises(ADAPTER.BootromDisconnectedError),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_permission_denial_reports_access_control(self) -> None:
        """An existing node rejected by the OS keeps the actionable udev diagnosis."""
        clock = FakeClock()
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=PermissionError),
            mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
            mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
            pytest.raises(SystemExit, match="not readable and writable"),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_transient_permission_denial_is_retried(self) -> None:
        """Allow the desktop uaccess ACL to arrive after USB enumeration."""
        clock = FakeClock()
        with (
            mock.patch.object(
                ADAPTER.os,
                "open",
                side_effect=[PermissionError, 41],
            ),
            mock.patch.object(ADAPTER.os, "close") as close_device,
            mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
            mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

        close_device.assert_called_once_with(41)

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
