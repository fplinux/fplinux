# SPDX-License-Identifier: GPL-2.0-only
"""Exercise enrolled card operations with real SSH and controlled USB discovery."""

from __future__ import annotations

import shlex
import shutil
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime import card_access
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.operations import LiveRequest, SelectedConnection, execute_operation
from fplinux_cli.runtime.ssh import session as ssh_session
from fplinux_cli.runtime.ssh import transport
from fplinux_cli.runtime.ssh.commands import ssh_argv

from tests.boot_identity import IDENTITY_9
from tests.host_tool.runtime.openssh_server import OpenSshServer
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Iterator


_CID = "0123456789abcdef0123456789abcdef"
_RELEASE = "bbbbbbbbbbbb"
_PEER = Path(__file__).with_name("fixtures") / "card-connection/peer.py"
_UNRELATED_PIN = "AAAAC3NzaC1lZDI1NTE5AAAAIHJycnJycnJycnJycnJycnJycnJycnJycnJycnJycnJy"


def _file(root: Path, relative: str, content: str, *, executable: bool = False) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="ascii")
    if executable:
        path.chmod(0o755)


def _phone(phone: Path, access: card_access.CardAccess, host_key: str, *, entry: str) -> None:
    _file(
        phone,
        "bin/uname",
        "#!/bin/sh\nprintf '6.18.42-fplinux-9999999999999999\\n'\n",
        executable=True,
    )
    _file(phone, "proc/device-tree/chosen/fplinux,boot-recipe", "9" * 64 + "\0")
    _file(phone, "proc/device-tree/chosen/fplinux,boot-entry", entry + "\0")
    _file(phone, "etc/os-release", "BUILD_ID=bbbbbbbbbbbb\n")
    _file(phone, "run/fplinux-card/system/target", "phone\n")
    _file(
        phone,
        "proc/self/mountinfo",
        "1 0 179:2 / ./run/fplinux-card rw - ext4 /dev/mmcblk0p2 rw\n",
    )
    _file(phone, "sys/devices/mmcblk0/device/type", "SD\n")
    _file(phone, "sys/devices/mmcblk0/device/cid", _CID + "\n")
    (phone / "sys/devices/mmcblk0/mmcblk0p2").mkdir()
    (phone / "sys/dev/block").mkdir(parents=True)
    (phone / "sys/dev/block/179:2").symlink_to("../../devices/mmcblk0/mmcblk0p2")
    _file(
        phone,
        "usr/libexec/fplinux/card-boot",
        '#!/bin/sh\n[ "$*" = inspect ] && cat slot\n',
        executable=True,
    )
    _file(phone, "slot", "slot=a\n")
    _file(phone, "usr/bin/dropbearkey", "#!/bin/sh\ncat card-host-key\n", executable=True)
    _file(phone, "card-host-key", f"ssh-ed25519 {host_key} root@phone\n")
    identity = phone / "etc/fplinux/card-access/device-id"
    identity.parent.mkdir(parents=True)
    shutil.copyfile(access.files[0], identity)


@pytest.fixture
def ssh_card(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> Iterator[tuple[OpenSshServer, card_access.CardAccess]]:
    """Enroll under a temporary user store and own the lifetime of the SSH peer."""
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "enrollments %"))
    monkeypatch.setattr(card_access, "host_route_networks", set)
    server = OpenSshServer(tmp_path / "sshd", tmp_path / "transient")
    server.start()
    try:
        with card_access.prepare_card_access(
            (None, {"network": "10.23.45.0/30"}), "phone", _CID, tmp_path
        ) as access:
            access.pin_host_key(server.host_key_line("unused").split()[2])
            yield server, access
    finally:
        server.stop()


@pytest.mark.parametrize(
    "case",
    [
        pytest.param(("sd", "correct", None), id="autonomous"),
        pytest.param(("usb", "correct", None), id="usb-assisted"),
        pytest.param(
            ("sd", "wrong-host-key", "Host key verification failed"), id="autonomous-pin"
        ),
        pytest.param(
            ("usb", "wrong-host-key", "does not match the saved card SSH enrollment"),
            id="usb-assisted-pin",
        ),
        pytest.param(("usb", "wrong-card", "SD card CID does not match --card"), id="cid"),
        pytest.param(
            ("usb", "wrong-enrollment", "does not match the saved card SSH enrollment"),
            id="wrong-enrollment",
        ),
        pytest.param(("sd", "unregistered", "is not enrolled for phone on this PC"), id="unknown"),
        pytest.param(("sd", "wrong-build", "root has a different build identity"), id="root"),
        pytest.param(("sd", "wrong-recipe", "different kernel identity or boot recipe"), id="fit"),
        pytest.param(("sd", "wrong-route", "does not use its USB-NCM interface"), id="route"),
        pytest.param(("sd", "diagnostic-console", None), id="other-build"),
        pytest.param(
            ("usb", "wrong-card-console", "SD card CID does not match --card"), id="refuse-console"
        ),
        pytest.param(("sd", "correct-screenshot", None), id="autonomous-screenshot"),
        pytest.param(("usb", "correct-screenshot", None), id="usb-assisted-screenshot"),
        pytest.param(
            ("sd", "wrong-host-key-screenshot", "Host key verification failed"),
            id="refuse-screenshot",
        ),
    ],
)
def test_card_operation_uses_the_selected_pinned_peer(
    ssh_card: tuple[OpenSshServer, card_access.CardAccess],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    case: tuple[str, str, str | None],
) -> None:
    entry, scenario, diagnostic = case
    server, access = ssh_card
    host_key = server.host_key_line("unused").split()[2]
    phone = tmp_path / "phone filesystem"
    _phone(phone, access, host_key, entry=entry)
    data = tmp_path / "runtime"
    serial = access.usb_serial
    client_key = access.files[2].read_text(encoding="ascii")
    if entry == "usb":
        session = create_ready_session(data / "session")
        serial = session["usb_serial"]
        Path(session["known_hosts"]).write_text(
            server.host_key_line(f"fplinux-{serial}"), encoding="ascii"
        )
        shutil.copyfile(server.client_key, session["private_key"])
        ssh_session.mark_current(session)
        _file(
            phone,
            "bin/fplinux-session-id",
            "#!/bin/sh\ncat session-id\n",
            executable=True,
        )
        _file(phone, "session-id", session["session_id"] + "\n")
        client_key = server.client_key.with_suffix(".pub").read_text(encoding="ascii").split()[1]
    command = shlex.join([sys.executable, str(_PEER), str(phone)])
    server.authorized_keys.write_text(
        f'command="{command}" ssh-ed25519 {client_key}\n', encoding="ascii"
    )

    if scenario.startswith("wrong-host-key"):
        access.pin_host_key(_UNRELATED_PIN)
    elif scenario.startswith("wrong-card"):
        _file(phone, "sys/devices/mmcblk0/device/cid", "f" * 32 + "\n")
    elif scenario == "wrong-enrollment":
        _file(phone, "etc/fplinux/card-access/device-id", "another card identity")
    elif scenario in {"wrong-build", "diagnostic-console"}:
        _file(phone, "etc/os-release", "BUILD_ID=cccccccccccc\n")
    elif scenario == "wrong-recipe":
        _file(phone, "proc/device-tree/chosen/fplinux,boot-recipe", "8" * 64 + "\0")

    def devices(session: dict[str, Any], **_options: bool) -> list[Path]:
        return [tmp_path / "usb-device"] if session["usb_serial"] == serial else []

    def loopback_ssh(
        session: dict[str, Any], remote: str | None = None, *, connect_timeout: int = 5
    ) -> list[str]:
        argv = ssh_argv(session, remote, connect_timeout=connect_timeout)
        destination = argv.index(f"root@{session['phone_address']}")
        argv[destination] = f"{server.user}@127.0.0.1"
        argv[1:1] = ["-p", str(server.port), "-o", "BindAddress=127.0.0.1"]
        return argv

    monkeypatch.setattr(transport, "_usb_devices", devices)
    monkeypatch.setattr(transport, "_ncm_interface", lambda *_args: "usb0")
    monkeypatch.setattr(transport, "_network_ready", lambda *_args: True)
    device = "competing-link" if scenario == "wrong-route" else "usb0"
    monkeypatch.setattr(transport, "_ip_json", lambda _args: [{"dev": device}])
    monkeypatch.setattr(transport, "ssh_argv", loopback_ssh)
    bundle = ValidatedBundle(
        tmp_path / "bundle",
        {"target": "phone", "profile": "microsd-uboot", "release_id": _RELEASE},
        "b" * 64,
        IDENTITY_9,
        None,
    )
    console = scenario.endswith("console")
    screenshot = scenario.endswith("screenshot")
    frame = tmp_path / "screen.ppm"
    if screenshot:
        _file(
            phone,
            "usr/libexec/fplinux/screenshot",
            "#!/bin/sh\nprintf 'P6\\n1 1\\n255\\n\\377\\000\\000'\n",
            executable=True,
        )
    allow_other_build = scenario == "diagnostic-console"
    request = LiveRequest(
        target="phone",
        card_cid="f" * 32 if scenario == "unregistered" else _CID,
        verify_only=not (console or screenshot),
        allow_other_build=allow_other_build,
        exec_command="touch executed" if console else None,
        screenshot=frame if screenshot else None,
    )
    pin = access.directory / "known_hosts"
    expected_pin = pin.read_bytes()

    status = execute_operation(
        request,
        SelectedConnection(None, data, tmp_path / "logs", None if allow_other_build else bundle),
    )

    error = capsys.readouterr().err
    if diagnostic is None:
        assert status == 0, error
        assert "microSD slot: a" in error
    else:
        assert status == 1
        assert diagnostic in error
    assert (phone / "executed").exists() == allow_other_build
    if screenshot and diagnostic is None:
        assert frame.read_bytes() == b"P6\n1 1\n255\n\xff\x00\x00"
    else:
        assert not frame.exists()
    assert pin.read_bytes() == expected_pin
