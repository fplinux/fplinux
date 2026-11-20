# SPDX-License-Identifier: GPL-2.0-only
"""Exercise installed-card USB and SSH scripts inside an isolated root filesystem."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path
    from subprocess import CompletedProcess

GADGET = ROOT / "alpine/aports/fplinux-usb-gadget"
SSH = ROOT / "alpine/aports/fplinux-ssh"
BOUNDARY = ROOT / "tests/host_process/rootfs/fixtures/card-usb-boundary.sh"
PUBLIC_KEY = b"AAAAC3NzaC1lZDI1NTE5AAAAIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f"
NETWORK = (
    b"usb_serial=630dcd2966c4336691125448bbb25b4f\n"
    b"phone_address=172.19.45.6\n"
    b"host_address=172.19.45.5\n"
    b"netmask=255.255.255.252\n"
    b"broadcast=172.19.45.7\n"
    b"device_mac=02:00:00:00:00:01\n"
    b"host_mac=02:00:00:00:00:02\n"
)


@pytest.fixture
def card_root(tmp_path: Path) -> Path:
    """Install the real entry points with replacements for the device and daemon boundaries."""
    for directory in (
        "bin",
        "dev",
        "lib",
        "libexec",
        "proc/device-tree/chosen",
        "proc/sys/net/ipv4",
        "run",
        "sbin",
        "sys/class/net/usb0",
        "sys/class/udc/controller",
        "sys/kernel/config/usb_gadget",
        "usr/bin",
        "usr/lib/ssh",
        "usr/libexec/fplinux",
        "usr/sbin",
    ):
        (tmp_path / directory).mkdir(parents=True)
    shutil.copy2("/bin/busybox", tmp_path / "libexec/busybox")
    shutil.copy2("/lib/ld-musl-x86_64.so.1", tmp_path / "lib/ld-musl-x86_64.so.1")
    for applet in (
        "awk",
        "cat",
        "chmod",
        "cmp",
        "dd",
        "grep",
        "mv",
        "od",
        "rm",
        "rmdir",
        "sed",
        "sha256sum",
        "sh",
        "stat",
        "tail",
        "tr",
        "uname",
        "wc",
    ):
        (tmp_path / "bin" / applet).symlink_to("/libexec/busybox")
    shutil.copy2(BOUNDARY, tmp_path / "libexec/boundary")
    for executable in (
        "bin/busybox",
        "bin/ln",
        "bin/mkdir",
        "sbin/ifconfig",
        "sbin/ip",
        "sbin/route",
        "usr/bin/dropbearkey",
        "usr/sbin/dropbear",
    ):
        (tmp_path / executable).symlink_to("/libexec/boundary")
    (tmp_path / "usr/lib/ssh/sftp-server").symlink_to("/bin/sh")
    for source, destination in (
        (GADGET / "fplinux-usb-gadget", "usr/libexec/fplinux/usb-gadget"),
        (GADGET / "fplinux-usb-network-policy.sh", "usr/libexec/fplinux/usb-network-policy.sh"),
        (GADGET / "fplinux-usb-transport.sh", "usr/libexec/fplinux/usb-transport.sh"),
        (SSH / "fplinux-ssh", "usr/libexec/fplinux/ssh-server"),
        (SSH / "fplinux-session-id", "usr/bin/fplinux-session-id"),
    ):
        shutil.copy2(source, tmp_path / destination)
    (tmp_path / "proc/device-tree/chosen/fplinux,boot-entry").write_bytes(b"sd\0")
    (tmp_path / "proc/sys/net/ipv4/ip_forward").write_bytes(b"0\n")
    # Device-status and shell redirections run against ordinary test files.
    (tmp_path / "dev/null").touch()
    return tmp_path


def provision_card(root: Path, transport: bytes = b"ncm") -> Path:
    access = root / "etc/fplinux/card-access"
    access.mkdir(mode=0o700, parents=True)
    for filename, contents in (
        ("device-id", bytes(range(32))),
        ("usb-network", (NETWORK + b"transport=" + transport + b"\n").ljust(256, b"\0")),
        ("client-key", PUBLIC_KEY),
        ("host-key", b"installed host key\n"),
    ):
        target = access / filename
        target.write_bytes(contents)
        target.chmod(0o600)
    return access


def run_entry(root: Path, program: str, *args: str) -> CompletedProcess[str]:
    return run_process(
        ["/bin/busybox", "chroot", str(root), program, *args],
        name=program,
        env={"PATH": "/bin", "LC_ALL": "C"},
        timeout=5,
    )


def test_dhcp_service_invokes_the_server_in_the_foreground(card_root: Path) -> None:
    """Source the shipped service in a root that also has its shared USB policy."""
    provision_card(card_root)
    service = card_root / "etc/init.d/fplinux-usb-dhcp"
    service.parent.mkdir()
    shutil.copyfile(GADGET / "fplinux-usb-dhcp.initd", service)
    busybox = card_root / "bin/busybox"
    busybox.unlink()
    shutil.copyfile(ROOT / "tests/host_process/rootfs/fixtures/busybox_dhcp.sh", busybox)
    busybox.chmod(0o755)
    (card_root / "run/fplinux-udhcpd.conf").write_text("interface usb0\n")

    result = run_entry(
        card_root,
        "/bin/sh",
        "-c",
        '. /etc/init.d/fplinux-usb-dhcp; exec "$command" $command_args',
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == "foreground DHCP started\n"


@pytest.mark.parametrize("transport", ["ncm", "rndis"])
def test_installed_card_configures_network_without_a_usb_host_or_keyboard(
    card_root: Path, transport: str
) -> None:
    provision_card(card_root, transport.encode("ascii"))

    result = run_entry(card_root, "/usr/libexec/fplinux/usb-gadget", "start")

    log = (card_root / "run/fplinux-usb-gadget.log").read_text()
    assert result.returncode == 0, log
    gadget = card_root / "sys/kernel/config/usb_gadget/fplinux"
    assert (gadget / "UDC").read_text() == "controller\n"
    assert (gadget / "strings/0x409/serialnumber").read_text() == (
        "630dcd2966c4336691125448bbb25b4f\n"
    )
    assert list((gadget / "functions").iterdir()) == [gadget / f"functions/{transport}.usb0"]
    assert (gadget / f"configs/c.1/{transport}.usb0").is_symlink()
    assert not (gadget / "configs/c.1/gser.keyboard").is_symlink()
    assert (card_root / "run/ifconfig").read_text().splitlines() == [
        "usb0 172.19.45.6 netmask 255.255.255.252 broadcast 172.19.45.7 up",
        "usb0",
    ]
    dhcp = (card_root / "run/fplinux-udhcpd.conf").read_text().splitlines()
    assert dhcp[:3] == ["start 172.19.45.5", "end 172.19.45.5", "interface usb0"]
    assert "option router" not in "\n".join(dhcp)


def test_installed_card_ssh_reuses_the_host_key_and_requires_the_installed_client_key(
    card_root: Path,
) -> None:
    access = provision_card(card_root)
    (card_root / "run/fplinux-usb-session.conf").write_text("phone_address=172.19.45.6\n")
    (card_root / "run/random-pending").touch()

    result = run_entry(card_root, "/usr/libexec/fplinux/ssh-server")

    assert result.returncode == 0, result.stderr
    assert (card_root / "run/dropbear").read_text().splitlines() == [
        "-F",
        "-E",
        "-r",
        "/etc/fplinux/card-access/host-key",
        "-D",
        "/run/fplinux-ssh",
        "-p",
        "172.19.45.6:22",
        "-s",
        "-j",
        "-k",
    ]
    authorized = card_root / "run/fplinux-ssh/authorized_keys"
    assert authorized.stat().st_mode & 0o777 == 0o600
    assert authorized.read_bytes().splitlines()[0].split()[:3] == [
        b"no-port-forwarding,no-agent-forwarding,no-X11-forwarding",
        b"ssh-ed25519",
        PUBLIC_KEY,
    ]
    assert (access / "host-key").read_bytes() == b"installed host key\n"
    assert not (card_root / "run/fplinux-ssh/dropbear_ed25519_host_key").exists()


def test_pc_assisted_ssh_uses_transient_host_key_and_device_tree_client(card_root: Path) -> None:
    access = provision_card(card_root)
    (card_root / "proc/device-tree/chosen/fplinux,boot-entry").write_bytes(b"ram\0")
    session = card_root / "proc/device-tree/fplinux-session"
    session.mkdir()
    session_client_key = PUBLIC_KEY[:-1] + b"g"
    (session / "fplinux,session-id").write_bytes(b"a" * 32)
    (session / "fplinux,ssh-client-key").write_bytes(session_client_key)
    (card_root / "run/fplinux-usb-session.conf").write_text("phone_address=172.19.45.6\n")

    result = run_entry(card_root, "/usr/libexec/fplinux/ssh-server")

    assert result.returncode == 0, result.stderr
    host_key = "run/fplinux-ssh/dropbear_ed25519_host_key"
    assert (card_root / "run/dropbear").read_text().splitlines()[2:4] == ["-r", f"/{host_key}"]
    assert (card_root / host_key).read_text() == "generated host key\n"
    assert (card_root / "run/fplinux-ssh/authorized_keys").read_bytes().split()[
        2
    ] == session_client_key
    assert (access / "host-key").read_bytes() == b"installed host key\n"


@pytest.mark.parametrize("insecure_key", [False, True], ids=["not-installed", "insecure-host-key"])
def test_card_boot_without_private_provisioning_leaves_usb_access_disabled(
    card_root: Path, *, insecure_key: bool
) -> None:
    if insecure_key:
        (provision_card(card_root) / "host-key").chmod(0o644)

    result = run_entry(card_root, "/usr/libexec/fplinux/usb-gadget", "start")

    assert result.returncode == 0
    assert not (card_root / "sys/kernel/config/usb_gadget/fplinux").exists()
    assert not (card_root / "run/fplinux-udhcpd.conf").exists()


def test_network_record_from_another_card_cannot_publish_usb_access(card_root: Path) -> None:
    access = provision_card(card_root)
    (access / "device-id").write_bytes(b"x" * 32)

    result = run_entry(card_root, "/usr/libexec/fplinux/usb-gadget", "start")

    assert result.returncode != 0
    log = (card_root / "run/fplinux-usb-gadget.log").read_text()
    assert "FAIL stage=validate_usb_session" in log
    assert not (card_root / "run/fplinux-udhcpd.conf").exists()


def test_autonomous_identity_does_not_appear_as_an_ephemeral_session(card_root: Path) -> None:
    provision_card(card_root)
    session = card_root / "proc/device-tree/fplinux-session"
    session.mkdir()
    (session / "fplinux,session-id").write_bytes(b"a" * 32)

    autonomous = run_entry(card_root, "/usr/bin/fplinux-session-id")

    assert autonomous.returncode != 0
    assert autonomous.stdout == ""
    (card_root / "proc/device-tree/chosen/fplinux,boot-entry").write_bytes(b"ram\0")
    assisted = run_entry(card_root, "/usr/bin/fplinux-session-id")
    assert assisted.returncode == 0
    assert assisted.stdout == "61" * 32 + "\n"
