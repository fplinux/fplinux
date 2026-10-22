# SPDX-License-Identifier: GPL-2.0-only
"""Connect on behalf of one live process and pass each socket to the test."""

import socket
import sys


def main() -> None:
    with socket.socket(fileno=0) as requests:
        while requests.recv(1):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.connect(sys.argv[1])
                socket.send_fds(requests, [b"c"], [client.fileno()])


if __name__ == "__main__":
    main()
