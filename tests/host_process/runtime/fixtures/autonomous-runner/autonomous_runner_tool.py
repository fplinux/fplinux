# SPDX-License-Identifier: GPL-2.0-only
"""Local RAM-tool, SSH, route and USB-rule process peers for the archive runner.

Every side effect is confined to the test archive. Unexpected privileged commands
fail before an external program or system path can be used.
"""

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


def _ssh_session(root: Path) -> dict[str, Any]:
    """Read only the session attached to the client key passed to this local peer."""
    option = next(argument for argument in sys.argv if argument.startswith("IdentityFile="))
    key = Path(shlex.split(option.partition("=")[2])[0])
    if not key.is_relative_to(root / "data/session"):
        message = "controlled SSH received a key outside the archive session"
        raise SystemExit(message)
    session: dict[str, Any] = json.loads(key.with_name("session.json").read_text(encoding="utf-8"))
    return session


def _ssh(root: Path, boundary: Path) -> None:
    """Return remote identity and clock responses, or record the interactive shell."""
    session = _ssh_session(root)
    mode_path = boundary / "reconnect-mode"
    mode = mode_path.read_text(encoding="ascii") if mode_path.exists() else "ready"
    command = sys.argv[-1]
    if command == "fplinux-session-id":
        print("00" * 32 if mode == "session-identity" else session["session_id"])
    elif command == "uname -r":
        suffix = "34" * 8 if mode == "kernel-identity" else "12" * 8
        print(f"6.18.42-fplinux-{suffix}")
    elif command.startswith("fplinux-clock "):
        print("system=1234 rtc=kept")
    elif command == "printf ready":
        print("ready", end="")
    elif "-tt" in sys.argv:
        with (boundary / "shells.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(session) + "\n")
        print("controlled interactive shell output")
    else:
        raise SystemExit(f"unexpected controlled SSH command: {command}")


def _sudo(root: Path, boundary: Path) -> None:
    """Emulate only installation into the owned rule path and its local reload peer."""
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
    """Choose one external executable boundary by its installed name."""
    root = Path(os.environ["FPLINUX_AUTONOMOUS_TEST_ROOT"])
    boundary = root / "boundary"
    tool = Path(sys.argv[0]).name
    if tool == "spd_dump.exe":
        image = Path(sys.argv[-2])
        if not image.is_relative_to(root / "data/session"):
            message = "controlled RAM loader received an image outside the archive session"
            raise SystemExit(message)
        shutil.copyfile(image, boundary / "loaded-image.bin")
        with (boundary / "ram-loads.txt").open("a", encoding="ascii") as stream:
            stream.write("RAM load\n")
        print("controlled RAM loader stdout", flush=True)
        print("controlled RAM loader stderr", file=sys.stderr, flush=True)
    elif tool == "libc_server.exe":
        received = boundary / "bridge-received"
        received.mkdir()
        for name in ("device-data.cpio",):
            shutil.copyfile(Path.cwd() / name, received / name)
        token = sys.argv[sys.argv.index("--fplinux-handoff") + 1]
        (boundary / "handoff-session.txt").write_text(token, encoding="ascii")
        (boundary / "bootrom").unlink()
        (boundary / "linux-usb").mkdir()
        print("controlled bridge stdout", flush=True)
        print("controlled bridge stderr", file=sys.stderr, flush=True)
    elif tool == "ssh":
        _ssh(root, boundary)
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
        print("controlled USB rules reloaded", flush=True)
    else:
        raise SystemExit(f"unexpected controlled tool: {tool}")


if __name__ == "__main__":
    main()
