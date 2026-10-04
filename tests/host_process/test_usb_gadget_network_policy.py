# SPDX-License-Identifier: GPL-2.0-only
"""Execute the sourced USB network policy with a controlled external route query."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "alpine/aports/fplinux-usb-gadget/fplinux-usb-network-policy.sh"
ROUTE_COMMAND = ROOT / "tests/fixtures/processes/usb_route_command.sh"
CONNECTED_ROUTE = "192.0.2.0/30 dev usb0 scope link src 192.0.2.1\n"


class UsbGadgetNetworkPolicyTests(unittest.TestCase):
    """Observe the production shell policy; no ConfigFS, phone or routing is exercised."""

    def run_policy(
        self, root: Path, *, routes: str, route_status: int = 0, forwarding: int = 0
    ) -> subprocess.CompletedProcess[str]:
        """Source the production library and replace only proc/IP external inputs."""
        forwarding_file = root / "ip_forward"
        forwarding_file.write_text(f"{forwarding}\n")
        route_table = root / "routes"
        route_table.write_text(routes)
        route_command = root / "ip"
        route_command.symlink_to(ROUTE_COMMAND)
        return subprocess.run(
            [
                "sh",
                "-c",
                'set -eu; . "$1"; verify_usb_forwarding "$2"; verify_usb_routes "$3"',
                "usb-network-policy",
                str(POLICY),
                str(forwarding_file),
                str(route_command),
            ],
            env=os.environ
            | {
                "LC_ALL": "C",
                "LANG": "C",
                "FPLINUX_TEST_ROUTE_TABLE": str(route_table),
                "FPLINUX_TEST_ROUTE_STATUS": str(route_status),
            },
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )

    def test_connected_usb_routes_and_pan_gateway_allow_local_usb(self) -> None:
        """An empty table, connected USB subnet and BNEP default satisfy the policy."""
        for routes in (
            "",
            CONNECTED_ROUTE,
            "default via 198.51.100.1 dev bnep0\n" + CONNECTED_ROUTE,
        ):
            with self.subTest(routes=routes), tempfile.TemporaryDirectory() as directory:
                result = self.run_policy(Path(directory), routes=routes)

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_default_route_through_usb_is_rejected(self) -> None:
        """Another default route does not make a USB gateway acceptable."""
        for routes in (
            "default via 192.0.2.2 dev usb0\n" + CONNECTED_ROUTE,
            "0.0.0.0/0 dev usb0\n" + CONNECTED_ROUTE,
            "default via 198.51.100.1 dev bnep0\n"
            "default via 192.0.2.2 dev usb0 metric 10\n" + CONNECTED_ROUTE,
        ):
            with self.subTest(routes=routes), tempfile.TemporaryDirectory() as directory:
                result = self.run_policy(Path(directory), routes=routes)

                self.assertNotEqual(result.returncode, 0)

    def test_failed_route_query_cannot_satisfy_network_policy(self) -> None:
        """A query failure remains visible even when stdout has no routes."""
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_policy(Path(directory), routes="", route_status=42)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("routing query failed", result.stderr)

    def test_ipv4_forwarding_is_rejected(self) -> None:
        """Allowed routes do not authorize forwarding between USB and PAN."""
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_policy(Path(directory), routes=CONNECTED_ROUTE, forwarding=1)

        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
