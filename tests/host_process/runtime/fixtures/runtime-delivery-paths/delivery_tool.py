# SPDX-License-Identifier: GPL-2.0-only
"""Local external processes for reader preparation, RAM loading and SSH reconnect."""

from __future__ import annotations

import base64
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


def _ssh_session() -> dict[str, Any]:
    """Accept only the real session selected by the caller's private client key."""
    option = next(argument for argument in sys.argv if argument.startswith("IdentityFile="))
    key = Path(shlex.split(option.partition("=")[2])[0])
    data = Path(os.environ["FPLINUX_RUNTIME_DELIVERY_DATA"])
    if not key.is_relative_to(data):
        message = "controlled SSH received a key outside its private phone store"
        raise SystemExit(message)
    session: dict[str, Any] = json.loads(key.with_name("session.json").read_text())
    return session


def _preparation_status(command: str, boundary: Path) -> None:
    """Accept only the preparation helper's named stages and measured NAND counters."""
    arguments = shlex.split(command)
    helper = "/usr/libexec/fplinux/preparation"
    status = "/run/fplinux-preparation-status"
    stage_commands = [
        [helper, "--stage", stage, status]
        for stage in ("waiting", "extracting", "ready", "error", "cancelled")
    ]
    reading = (
        len(arguments) == 8
        and arguments[:4] == [helper, "--stage", "reading", "--completed"]
        and arguments[5:] == ["--total", "138412032", status]
        and arguments[4].isascii()
        and arguments[4].isdecimal()
        and 0 <= int(arguments[4]) <= 138412032
    )
    if arguments not in stage_commands and not reading:
        message = f"unexpected controlled preparation command: {command}"
        raise SystemExit(message)
    with (boundary / "preparation-commands").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(arguments) + "\n")


def _ssh(boundary: Path) -> None:
    """Return protocol responses or stream the retained physical NAND fixture as remote stdout."""
    session = _ssh_session()
    command = sys.argv[-1]
    if command == "fplinux-session-id":
        print(session["session_id"])
    elif command == "uname -r":
        print("6.18.42-fplinux-1212121212121212")
    elif command.startswith("fplinux-clock "):
        with (boundary / "clock-commands").open("a", encoding="ascii") as stream:
            stream.write(command + "\n")
        print("system=1234 rtc=kept")
    elif command.startswith("/usr/libexec/fplinux/preparation "):
        _preparation_status(command, boundary)
    elif command in (
        "cat /sys/class/misc/raw-nand/device/geometry 3</dev/raw-nand",
        "exec dd if=/dev/raw-nand bs=63360",
    ):
        with (boundary / "reader-commands").open("a", encoding="utf-8") as stream:
            stream.write(command + "\n")
        if command.startswith("cat "):
            sys.stdout.buffer.write((boundary / "geometry").read_bytes())
        else:
            with Path(os.environ["FPLINUX_RUNTIME_DELIVERY_NAND"]).open("rb") as source:
                shutil.copyfileobj(source, sys.stdout.buffer)
    elif command == "cat /proc/mounts && printf '\\nFPLINUX_SWAPS\\n' && cat /proc/swaps":
        print("rootfs / rootfs rw 0 0")
        print("/dev/loop0 /run/fplinux-root/lower squashfs ro,relatime 0 0")
        print("\nFPLINUX_SWAPS")
        print("Filename Type Size Used Priority")
        print("/dev/zram0 partition 65532 0 -2")
    elif "-tt" in sys.argv:
        with (boundary / "shells.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(session) + "\n")
        print("controlled interactive shell output")
    else:
        raise SystemExit(f"unexpected controlled SSH command: {command}")


def _sudo(root: Path, boundary: Path) -> None:
    """Permit only the owned USB-rule installation and its local reload process."""
    arguments = sys.argv[1:]
    if arguments[:3] == ["install", "-m", "0644"] and len(arguments) == 5:
        source, destination = map(Path, arguments[3:])
        if source != root / "60-fplinux.rules" or destination != boundary / "usb-access.rules":
            message = "controlled USB setup refused a path outside its owned rule file"
            raise SystemExit(message)
        shutil.copyfile(source, destination)
        destination.chmod(0o644)
    elif arguments == [str(root / "bin/udevadm"), "control", "--reload-rules"]:
        result = subprocess.run(arguments, check=False)
        raise SystemExit(result.returncode)
    else:
        message = "unexpected controlled privileged command"
        raise SystemExit(message)


def main() -> None:
    """Dispatch external tool behavior by its installed executable name."""
    root = Path(os.environ["FPLINUX_AUTONOMOUS_TEST_ROOT"])
    boundary = root / "boundary"
    tool = Path(sys.argv[0]).name
    if tool == "spd_dump.exe":
        data = Path(os.environ["FPLINUX_RUNTIME_DELIVERY_DATA"])
        image = Path(sys.argv[-2])
        if not image.is_relative_to(data):
            message = "controlled RAM loader received an image outside its private phone store"
            raise SystemExit(message)
        with (boundary / "ram-loads").open("a", encoding="ascii") as stream:
            stream.write("loaded\n")
        print("controlled RAM loader output", flush=True)
    elif tool == "libc_server.exe":
        capsules = boundary / "capsules"
        capsules.mkdir(exist_ok=True)
        number = len(list(capsules.iterdir())) + 1
        shutil.copyfile(Path.cwd() / "device-data.cpio", capsules / f"{number}.cpio")
        (boundary / "bootrom").unlink()
        (boundary / "linux-usb").mkdir()
        print("controlled bridge output", flush=True)
    elif tool == "ssh":
        _ssh(boundary)
    elif tool == "ssh-keyscan":
        public_blob = b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + b"k" * 32
        key_body = base64.b64encode(public_blob).decode("ascii")
        print(f"{sys.argv[-1]} ssh-ed25519 {key_body}")
    elif tool == "ip":
        if sys.argv[1:] != ["-4", "-j", "route", "show", "table", "all"]:
            message = "unexpected controlled route command"
            raise SystemExit(message)
        print("[]")
    elif tool == "sudo":
        _sudo(root, boundary)
    elif tool == "udevadm":
        if sys.argv[1:] != ["control", "--reload-rules"]:
            message = "unexpected controlled USB rule command"
            raise SystemExit(message)
        (boundary / "rules-reloaded").write_text("reloaded\n", encoding="ascii")
    else:
        raise SystemExit(f"unexpected controlled tool: {tool}")


if __name__ == "__main__":
    main()
