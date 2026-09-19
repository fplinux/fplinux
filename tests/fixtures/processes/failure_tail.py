# SPDX-License-Identifier: GPL-2.0-only
"""Emit a long log with terminal escapes and invalid bytes before failing."""

import sys


def main() -> None:
    for index in range(100):
        sys.stdout.write(f"line-{index:03d}\n")
    sys.stdout.flush()
    sys.stdout.buffer.write(b"\x1b[31mred\x1b[0m invalid=\xff\n")
    sys.stdout.buffer.write("utf8=проверка\n".encode())
    raise SystemExit(1)


if __name__ == "__main__":
    main()
