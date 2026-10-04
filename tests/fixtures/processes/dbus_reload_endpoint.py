#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fake one D-Bus policy reload endpoint for service-hook process tests."""

import argparse
import os
import sys
from pathlib import Path


def main() -> int:
    """Model policy availability after a valid reload, or an endpoint rejection."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--system", action="store_true")
    parser.add_argument("--type", dest="message_type")
    parser.add_argument("--print-reply", action="store_true")
    parser.add_argument("--dest", dest="destination")
    parser.add_argument("object_path")
    parser.add_argument("method")
    arguments = parser.parse_args()
    if not all(
        (
            arguments.system,
            arguments.message_type == "method_call",
            arguments.print_reply,
            arguments.destination == "org.freedesktop.DBus",
            arguments.object_path == "/org/freedesktop/DBus",
            arguments.method == "org.freedesktop.DBus.ReloadConfig",
        )
    ):
        print("unsupported policy reload request", file=sys.stderr)
        return 2
    if os.environ.get("FPLINUX_TEST_RELOAD_FAILURE") == "1":
        print("policy reload rejected", file=sys.stderr)
        return 42
    Path(os.environ["FPLINUX_TEST_POLICY_READY"]).write_text("ready\n")
    print("method return")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
