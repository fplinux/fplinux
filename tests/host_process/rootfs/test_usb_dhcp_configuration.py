# SPDX-License-Identifier: GPL-2.0-only
"""Execute DHCP shell configuration with a controlled external server."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from tests import ROOT

SERVICE = ROOT / "alpine/aports/fplinux-usb-gadget/fplinux-usb-dhcp.initd"


class UsbDhcpConfigurationTests:
    """The configured command must resolve and keep DHCP in the foreground."""

    def test_configuration_invokes_available_dhcp_server_in_foreground(self) -> None:
        """A shell-loaded configuration invokes the server with its runtime file."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "bin/busybox"
            binary.parent.mkdir()
            # This fake replaces the external DHCP server; no sockets are opened.
            binary.write_text(
                "#!/bin/sh\n"
                '[ "$1" = udhcpd ] || exit 9\n'
                "shift\n"
                '[ "$#" = 2 ] && [ "$1" = -f ] || exit 10\n'
                '[ "$2" = /run/fplinux-udhcpd.conf ] || exit 11\n'
                "printf 'foreground DHCP started\\n'\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)
            applet = root / "usr/sbin/udhcpd"
            applet.parent.mkdir(parents=True)
            applet.write_text(
                '#!/bin/sh\nexec "$(dirname "$0")/../../bin/busybox" udhcpd "$@"\n',
                encoding="utf-8",
            )
            applet.chmod(0o755)
            result = subprocess.run(
                [
                    "sh",
                    "-c",
                    '. "$2"; exec "$1$command" $command_args',
                    "dhcp-config",
                    str(root),
                    str(SERVICE),
                ],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == ("foreground DHCP started\n")
