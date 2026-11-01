# SPDX-License-Identifier: GPL-2.0-only
"""A private D-Bus daemon standing in for the system bus of one case."""

from __future__ import annotations

import os
import select
import signal
import subprocess
import tempfile
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

# No service directories: a request to an absent name fails instead of
# activating a service installed on the host.
BUS_CONFIG = ROOT / "tests/host_tool/media/fixtures/private-bus.conf.in"
BLUEZ_FAKE = ROOT / "tests/host_tool/gui/bluez-fake.c"


def _dbus_pkg_config(option: str) -> list[str]:
    return run_process(
        ["pkg-config", option, "dbus-1"],
        name=f"read D-Bus {option[2:]}",
        timeout=10,
        check=True,
    ).stdout.split()


def dbus_flags() -> tuple[list[str], list[str]]:
    """Compiler and linker flags of the pinned libdbus."""
    return _dbus_pkg_config("--cflags"), _dbus_pkg_config("--libs")


@dataclass
class PrivateBus:
    """The daemon's address and process, owned by one case."""

    address: str
    process: subprocess.Popen[str]

    def environment(self) -> dict[str, str]:
        """Point a child's system bus at this daemon only."""
        return os.environ | {"DBUS_SYSTEM_BUS_ADDRESS": self.address}

    def stop(self) -> None:
        """End the daemon, as when the system bus goes away."""
        if self.process.poll() is not None:
            self.process.communicate()
            return
        with suppress(ProcessLookupError):
            os.killpg(self.process.pid, signal.SIGTERM)
        try:
            self.process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGKILL)
            self.process.communicate()


@contextmanager
def private_bus() -> Iterator[PrivateBus]:
    """Start a daemon in a short-lived directory with a short socket path."""
    with tempfile.TemporaryDirectory(prefix="fplinux-bus-") as temporary:
        configuration = Path(temporary) / "bus.conf"
        configuration.write_text(
            BUS_CONFIG.read_text(encoding="utf-8").format(directory=temporary),
            encoding="utf-8",
        )
        process = subprocess.Popen(
            ["dbus-daemon", f"--config-file={configuration}", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        bus = PrivateBus("", process)
        try:
            assert process.stdout is not None
            readable, _, _ = select.select([process.stdout], [], [], 3)
            bus.address = process.stdout.readline().strip() if readable else ""
            assert bus.address, "the private D-Bus daemon published no address"
            yield bus
        finally:
            bus.stop()
