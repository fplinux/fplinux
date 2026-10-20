# SPDX-License-Identifier: GPL-2.0-only
"""Small behavior tests for session-bound USB-NCM SSH transport."""

from __future__ import annotations

import hashlib
import ipaddress
import itertools
import json
import shlex
import subprocess
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from typing import Any
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport

from tests import ROOT
from tests.bundle_support import file_record
from tests.ssh_transport_support import create_ready_session


def ieee_crc32(data: bytes) -> int:
    """Compute the Ethernet CRC-32 independently of the transport implementation."""
    remainder = 0xFFFFFFFF
    for byte in data:
        remainder ^= byte
        for _bit in range(8):
            remainder = (remainder >> 1) ^ (0xEDB88320 if remainder & 1 else 0)
    return remainder ^ 0xFFFFFFFF


def write_bundle(bundle: Path) -> tuple[dict[str, Any], str]:
    """Write a default-profile ``phone`` bundle whose build manifest binds its runtime files.

    The manifest records device identity ``"9" * 64``. Return the runtime manifest and the
    generation recorded in the build manifest.
    """
    image = bundle / "image/ramboot.bin"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"DHTB image\n")
    runtime: dict[str, Any] = {
        "target": "phone",
        "profile": None,
        "build_type": "release",
        "image": "image/ramboot.bin",
        "sha256": {"image/ramboot.bin": hashlib.sha256(image.read_bytes()).hexdigest()},
    }
    runtime_path = bundle / "runtime-manifest.json"
    runtime_path.write_text(json.dumps(runtime, sort_keys=True) + "\n", encoding="utf-8")

    payload = {
        "rootfs_receipt": {"recipe": "5" * 64, "sha256": "6" * 64},
        "kernel_artifacts": {},
        "boot_artifacts": {"required": []},
        "environment_recipe": "7" * 64,
        "environment_content": "4" * 64,
        "apk_signing_key": "8" * 64,
        "device_identity": "9" * 64,
        "files": {
            "image/ramboot.bin": file_record(image),
            "runtime-manifest.json": file_record(runtime_path),
        },
        "kbuild_receipt": {"recipe": "a" * 64, "sha256": "b" * 64},
        "linux_recipe": "c" * 64,
        "profile": None,
        "build_type": "release",
        "target": "phone",
        "workspace_digest": "d" * 64,
    }
    canonical = (
        json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode()
    generation = hashlib.sha256(canonical).hexdigest()
    (bundle / "build-manifest.json").write_text(
        json.dumps({**payload, "generation": generation}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return runtime, generation


class ReadyAfterPolls:
    """Stand in for a readiness probe that fails a fixed number of polls, then stays ready."""

    def __init__(self, failed_polls: int) -> None:
        """Set how many initial polls answer not ready."""
        self.failed_polls = failed_polls
        self.polls = 0

    def __call__(self, *_arguments: object) -> bool:
        """Answer one poll."""
        self.polls += 1
        return self.has_reported_ready()

    def has_reported_ready(self) -> bool:
        """Return whether any poll so far has answered ready."""
        return self.polls > self.failed_polls


@dataclass(frozen=True)
class SavedSession:
    """Ready saved state and a controlled USB attribute tree for one selector case."""

    root: Path
    state: dict[str, Any]
    current_path: Path
    usb: Path

    def usb_device(
        self,
        name: str,
        *,
        vendor: str = "0525",
        product: str = "a4a6",
        serial: str | None = None,
    ) -> Path:
        """Write test-owned kernel USB attributes for one controlled device."""
        device = self.usb / name
        device.mkdir()
        (device / "idVendor").write_text(vendor + "\n", encoding="ascii")
        (device / "idProduct").write_text(product + "\n", encoding="ascii")
        (device / "serial").write_text(
            (self.state["usb_serial"] if serial is None else serial) + "\n", encoding="ascii"
        )
        return device


@pytest.fixture
def saved_session(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> SavedSession:
    """Replace USB discovery while leaving state validation and selection real."""
    root = tmp_path / "runtime"
    root.mkdir(mode=0o700)
    state = create_ready_session(root)
    state["runtime_root"] = str(root)
    current = root / "current"
    current.mkdir(mode=0o700)
    current_path = current / "phone.json"
    current_path.write_text(json.dumps(state), encoding="utf-8")
    current_path.chmod(0o600)
    usb = tmp_path / "usb"
    usb.mkdir()

    def selected_path(value: str | Path) -> Path:
        return usb if value == "/sys/bus/usb/devices" else Path(value)

    monkeypatch.setattr(ssh_transport, "Path", selected_path)
    monkeypatch.delenv("XDG_RUNTIME_DIR", raising=False)
    return SavedSession(root, state, current_path, usb)


class LiveSessionSmallTests:
    """Observe session selection using private files and a controlled USB attribute tree."""

    def test_missing_saved_session_allows_a_new_load_without_weakening_current_loader(
        self,
        saved_session: SavedSession,
    ) -> None:
        """Only the optional live selector accepts the absence of a saved current pointer."""
        saved_session.current_path.unlink()

        assert ssh_transport.load_live_session("phone", runtime_root=saved_session.root) is None
        with pytest.raises(SystemExit, match="no ready SSH session for phone"):
            ssh_transport.load_current_session("phone", runtime_root=saved_session.root)

    def test_absent_exact_usb_allows_a_new_load_and_retains_saved_session(
        self, saved_session: SavedSession
    ) -> None:
        """Other USB identities do not select the saved RAM session or erase its private files."""
        saved_session.usb_device("other-vendor", vendor="1234")
        saved_session.usb_device("other-product", product="a4a7")
        saved_session.usb_device("other-session", serial="a" * 32)
        (saved_session.usb / "1-1:1.0").mkdir()

        assert ssh_transport.load_live_session("phone", runtime_root=saved_session.root) is None
        assert (
            ssh_transport.load_current_session("phone", runtime_root=saved_session.root)
            == saved_session.state
        )
        assert Path(saved_session.state["private_key"]).read_bytes() == b"private\n"
        assert Path(saved_session.state["known_hosts"]).read_bytes() == b"private\n"

    def test_present_exact_usb_selects_the_saved_session_before_endpoint_readiness(
        self, saved_session: SavedSession
    ) -> None:
        """A matching USB device selects reuse even before it exposes a network interface."""
        saved_session.usb_device("1-1")

        selected = ssh_transport.load_live_session("phone", runtime_root=saved_session.root)

        assert selected == saved_session.state
        assert saved_session.current_path.read_text(encoding="utf-8") == json.dumps(
            saved_session.state
        )

    @pytest.mark.parametrize("invalid_state", ["{", '{"unexpected": "value"}'])
    def test_invalid_saved_session_fails_instead_of_allowing_a_new_load(
        self, saved_session: SavedSession, invalid_state: str
    ) -> None:
        """Malformed or incomplete saved state remains an error even with no matching USB."""
        saved_session.current_path.write_text(invalid_state, encoding="utf-8")

        with pytest.raises(SystemExit, match="current SSH session state"):
            ssh_transport.load_live_session("phone", runtime_root=saved_session.root)

    def test_nonregular_saved_session_fails_instead_of_allowing_a_new_load(
        self, saved_session: SavedSession
    ) -> None:
        """An existing current path that is not a regular state file is not physical absence."""
        saved_session.current_path.unlink()
        saved_session.current_path.mkdir()

        with pytest.raises(SystemExit, match="session state is not a regular file"):
            ssh_transport.load_live_session("phone", runtime_root=saved_session.root)

    def test_ambiguous_exact_usb_fails_instead_of_allowing_a_new_load(
        self, saved_session: SavedSession
    ) -> None:
        """Two devices carrying the saved exact identity cannot select a reload or reconnect."""
        saved_session.usb_device("1-1")
        saved_session.usb_device("1-2")

        with pytest.raises(SystemExit, match="more than one USB device matches"):
            ssh_transport.load_live_session("phone", runtime_root=saved_session.root)

    def test_unavailable_usb_discovery_fails_instead_of_allowing_a_new_load(
        self, saved_session: SavedSession
    ) -> None:
        """An unavailable USB attribute tree cannot establish that the phone is absent."""
        saved_session.usb.rmdir()

        with pytest.raises(SystemExit, match="cannot discover the current USB session"):
            ssh_transport.load_live_session("phone", runtime_root=saved_session.root)

    @pytest.mark.parametrize(
        "unreadable_attribute", ["vendor", "missing-serial", "nonascii-serial"]
    )
    def test_unreadable_usb_identity_fails_instead_of_allowing_a_new_load(
        self, saved_session: SavedSession, unreadable_attribute: str
    ) -> None:
        """Failed USB identity parsing cannot be treated as proof of the session's absence."""
        device = saved_session.usb_device("1-1")
        if unreadable_attribute == "vendor":
            (device / "idVendor").write_text("unreadable\n", encoding="ascii")
        elif unreadable_attribute == "missing-serial":
            (device / "serial").unlink()
        else:
            (device / "serial").write_bytes(b"\xff\n")

        with pytest.raises(SystemExit, match="cannot discover the current USB session"):
            ssh_transport.load_live_session("phone", runtime_root=saved_session.root)


@dataclass(frozen=True)
class SshRuntime:
    """Private runtime path and adjacent bundle/transfer files for one case."""

    directory: Path
    root: Path

    def session(self, *, status: str = "ready") -> dict[str, Any]:
        """Create a session and private credentials beneath this case's runtime root."""
        return create_ready_session(self.root, status=status)


@pytest.fixture
def ssh_runtime(tmp_path: Path) -> SshRuntime:
    """Create one private user runtime root."""
    root = tmp_path / "runtime"
    root.mkdir(mode=0o700)
    return SshRuntime(tmp_path, root)


class SshTransportSmallTests:
    """Exercise in-process session, bundle and transfer boundaries."""

    def test_run_shell_rejects_a_noninteractive_process(self, ssh_runtime: SshRuntime) -> None:
        """Do not launch forced-PTY SSH when the host has no input terminal."""
        session = ssh_runtime.session()
        with (
            mock.patch("fplinux_cli.runtime.ssh_transport.os.isatty", return_value=False),
            mock.patch("fplinux_cli.runtime.ssh_transport.subprocess.Popen") as execute,
            pytest.raises(SystemExit, match="interactive SSH requires a terminal"),
        ):
            ssh_transport.run_shell(session)

        execute.assert_not_called()

    def test_bundle_identity_rejects_a_runtime_image_outside_its_build_manifest(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Refuse reconnect state when the RAM payload no longer matches the generation."""
        bundle = ssh_runtime.directory / "bundle"
        runtime, generation = write_bundle(bundle)

        identity = ssh_transport.bundle_identity(bundle, runtime)
        assert (identity) == ({"bundle_generation": generation})
        assert (ssh_transport.build_manifest_device_identity(bundle)) == ("9" * 64)

        (bundle / "image/ramboot.bin").write_bytes(b"DHTB changed\n")
        with pytest.raises(SystemExit, match="runtime closure differs"):
            ssh_transport.bundle_identity(bundle, runtime)

    @pytest.mark.parametrize(
        "changed",
        [
            pytest.param({"profile": "usb-host-lab"}, id="profile"),
            pytest.param({"build_type": "debug"}, id="build-type"),
        ],
    )
    def test_bundle_identity_rejects_a_runtime_from_another_profile(
        self, ssh_runtime: SshRuntime, changed: dict[str, str]
    ) -> None:
        """A named profile cannot reuse the default bundle's SSH identity."""
        bundle = ssh_runtime.directory / "bundle"
        runtime, _generation = write_bundle(bundle)

        with (
            pytest.raises(SystemExit, match="runtime target, profile or build type"),
        ):
            ssh_transport.bundle_identity(bundle, {**runtime, **changed})

    def test_current_session_loads_ready_bound_session(self, ssh_runtime: SshRuntime) -> None:
        """A ready RAM session remains usable through its private binding."""
        state = ssh_runtime.session()
        current = ssh_runtime.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)

        loaded = ssh_transport.load_current_session("phone", runtime_root=ssh_runtime.root)

        assert (loaded) == (state)

    def test_device_identity_accepts_the_running_selected_kernel(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Accept an authenticated session when uname identifies the selected runtime."""
        session = ssh_runtime.session()
        device_identity = "9" * 64
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout=f"6.12-fplinux-{device_identity[:16]}\n",
            stderr="",
        )
        with mock.patch.object(ssh_transport, "run_remote", return_value=result):
            release = ssh_transport.require_device_identity(session, device_identity)

        assert (release) == (f"6.12-fplinux-{device_identity[:16]}")

    def test_device_identity_rejects_a_different_running_kernel(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Reject a ready authenticated session running another device runtime."""
        session = ssh_runtime.session()
        result = subprocess.CompletedProcess(
            [],
            0,
            stdout="6.12-fplinux-aaaaaaaaaaaaaaaa\n",
            stderr="",
        )
        with (
            mock.patch.object(ssh_transport, "run_remote", return_value=result),
            pytest.raises(SystemExit, match="different kernel identity"),
        ):
            ssh_transport.require_device_identity(session, "9" * 64)

    def test_device_identity_rejects_a_failed_kernel_probe(self, ssh_runtime: SshRuntime) -> None:
        """A matching-looking stdout cannot hide a failed authenticated probe."""
        session = ssh_runtime.session()
        result = subprocess.CompletedProcess(
            [],
            7,
            stdout=f"6.12-fplinux-{'9' * 16}\n",
            stderr="transport failed\n",
        )
        with (
            mock.patch.object(ssh_transport, "run_remote", return_value=result),
            pytest.raises(SystemExit, match=r"running kernel identity \(exit 7\)"),
        ):
            ssh_transport.require_device_identity(session, "9" * 64)

    def test_current_session_rejects_an_unknown_field(self, ssh_runtime: SshRuntime) -> None:
        """An unrecognized host-state record is not a reconnect session."""
        state = {**ssh_runtime.session(), "unexpected": "value"}
        current = ssh_runtime.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)

        with (
            pytest.raises(SystemExit, match="unexpected fields"),
        ):
            ssh_transport.load_current_session("phone", runtime_root=ssh_runtime.root)

    def test_usb_session_text_is_exactly_seven_fields_with_nul_padding(self) -> None:
        """Emit the canonical fixed USB-gadget consumer record."""
        config = ssh_transport._usb_config(  # noqa: SLF001
            "0123456789abcdef0123456789abcdef",
            ipaddress.IPv4Network("10.23.45.0/30"),
            "02:00:00:00:00:01",
            "02:00:00:00:00:02",
        )
        expected = (ROOT / "tests/fixtures/ssh_transport/usb-session.txt").read_bytes()

        assert (config) == (expected.ljust(256, b"\0"))

    def test_session_block_uses_current_fixed_abi_header(self) -> None:
        """Emit the exact RAM-session header consumed by the bundled bootstrap."""
        session_id = bytes(range(32))
        rng_seed = bytes(range(64))
        public_key = b"A" * 68
        usb_config = b"usb_serial=0123456789abcdef0123456789abcdef\n".ljust(256, b"\0")

        block = ssh_transport._session_block(  # noqa: SLF001
            session_id,
            rng_seed,
            public_key,
            usb_config,
        )

        assert (len(block)) == (512)
        assert (block[:8]) == (b"FPLSESS\0")
        assert (block[8:12]) == (b"\0" * 4)
        assert (block[12:16]) == ((512).to_bytes(4, "little"))
        assert (block[16:48]) == (session_id)
        assert (block[48:112]) == (rng_seed)
        assert (block[112:180]) == (public_key)
        assert (block[180:436]) == (usb_config)
        assert (block[-4:]) == (ieee_crc32(block[:-4]).to_bytes(4, "little"))

    def test_reacquire_retries_mocked_usb_ncm_and_ssh_boundaries(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Retry transient failures reported by controlled USB, NCM, and SSH boundaries."""
        state = ssh_runtime.session()
        failures = [
            subprocess.CompletedProcess([], 255, stdout="", stderr="not ready"),
            subprocess.CompletedProcess([], 0, stdout=f"{state['session_id']}\n", stderr=""),
        ]
        with (
            mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb1"),
            mock.patch.object(ssh_transport, "_network_ready", return_value=True),
            mock.patch.object(ssh_transport, "_retry_pause"),
            mock.patch.object(ssh_transport, "_ssh_argv", return_value=["ssh"]),
            mock.patch("fplinux_cli.runtime.ssh_transport.subprocess.run", side_effect=failures),
        ):
            ready = ssh_transport.reacquire_bound_session(state)
            ssh_transport.finish_session(state)

        assert (ready["interface"]) == ("usb1")
        assert (ready["session_id"]) == (state["session_id"])
        assert Path(state["private_key"]).is_file()

    def test_fresh_session_reports_usb_once_before_network_and_ssh_are_ready(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """A matched USB device is observable even while network and SSH retry."""
        state = ssh_runtime.session()
        observations: list[tuple[str, bool]] = []
        network = ReadyAfterPolls(failed_polls=1)
        # The first SSH attempt fails; every later one proves the session identity.
        ssh_attempts = itertools.chain(
            [subprocess.CompletedProcess([], 255, stdout="", stderr="not ready")],
            itertools.repeat(
                subprocess.CompletedProcess([], 0, stdout=f"{state['session_id']}\n", stderr="")
            ),
        )
        # The phone is absent on the first USB poll and present on every later one.
        usb_polls = itertools.chain([[]], itertools.repeat([Path("/usb/phone")]))
        with (
            mock.patch.object(ssh_transport, "_usb_devices", side_effect=usb_polls),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb1"),
            mock.patch.object(ssh_transport, "_network_ready", side_effect=network),
            mock.patch.object(ssh_transport, "_retry_pause"),
            mock.patch.object(ssh_transport, "_scan_host_key", return_value=True),
            mock.patch.object(ssh_transport, "_ssh_argv", return_value=["ssh"]),
            mock.patch(
                "fplinux_cli.runtime.ssh_transport.subprocess.run", side_effect=ssh_attempts
            ),
        ):
            ready = ssh_transport.wait_for_bound_session(
                state,
                on_linux_usb=lambda: observations.append(
                    ("linux-usb", network.has_reported_ready())
                ),
            )
            ssh_transport.finish_session(state)

        assert (observations) == ([("linux-usb", False)])
        assert (ready["interface"]) == ("usb1")
        assert (ready["session_id"]) == (state["session_id"])

    @pytest.mark.parametrize(
        ("devices", "diagnostic"),
        [
            pytest.param([], "did not become ready", id="absent"),
            pytest.param(
                [Path("/usb/one"), Path("/usb/two")], "more than one USB device", id="ambiguous"
            ),
        ],
    )
    def test_absent_or_ambiguous_session_usb_is_not_reported(
        self, ssh_runtime: SshRuntime, devices: list[Path], diagnostic: str
    ) -> None:
        """Only one USB device matching the selected session can report arrival."""
        state = {**ssh_runtime.session(), "wait_seconds": 1}
        observations: list[str] = []
        with (
            mock.patch.object(ssh_transport, "_usb_devices", return_value=devices),
            mock.patch.object(ssh_transport, "_retry_pause"),
            # Each clock read advances half a second, so the 1-second wait expires.
            mock.patch(
                "fplinux_cli.runtime.ssh_transport.time.monotonic",
                side_effect=itertools.count(0.0, 0.5),
            ),
            pytest.raises(SystemExit, match=diagnostic),
        ):
            ssh_transport.wait_for_bound_session(
                state, on_linux_usb=partial(observations.append, "linux-usb")
            )
        assert (observations) == ([])

    def test_failed_current_config_publication_removes_the_ready_pointer_and_session(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Never leave a direct config pointing at an incomplete ready session."""
        session = ssh_runtime.session()
        write_private_text = ssh_transport._write_private_text  # noqa: SLF001

        def fail_config(path: Path, value: str) -> None:
            if path.name == "phone.ssh-config":
                message = "disk full"
                raise OSError(message)
            write_private_text(path, value)

        with (
            mock.patch.object(ssh_transport, "_write_private_text", side_effect=fail_config),
            pytest.raises(OSError, match="disk full"),
        ):
            ssh_transport._mark_current(session)  # noqa: SLF001

        current = ssh_runtime.root / "current"
        assert not ((current / "phone.json").exists())
        assert not ((current / "phone.ssh-config").exists())
        assert (list((ssh_runtime.root / "sessions").iterdir())) == ([])

    def test_cleanup_removes_only_the_selected_target_session(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Preserve another target's keys and unrelated files during target cleanup."""
        selected = create_ready_session(ssh_runtime.root)
        other = create_ready_session(ssh_runtime.root, target="other-phone")
        unrelated = ssh_runtime.root / "sessions" / "unrelated"
        unrelated.mkdir()
        unrelated_file = unrelated / "keep.txt"
        unrelated_file.write_bytes(b"unrelated contents\n")

        ssh_transport._cleanup_target_sessions(ssh_runtime.root, "phone")  # noqa: SLF001

        assert not Path(selected["private_key"]).parent.exists()
        assert Path(other["private_key"]).read_text(encoding="ascii") == "private\n"
        assert unrelated_file.read_bytes() == b"unrelated contents\n"

    def test_failed_prepare_erases_keys_and_invalidates_prior_current(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """A failed session preparation leaves no usable pointer or prepared key directory."""
        prior = ssh_runtime.session()
        current = ssh_runtime.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(prior), encoding="utf-8")
        image = ssh_runtime.directory / "ramboot.bin"
        image.write_bytes(b"DHTB" + b"\0" * 1532)
        descriptor = {
            "offset": 1024,
            "bytes": ssh_transport.SESSION_BYTES,
            "template_sha256": hashlib.sha256(b"\0" * 512).hexdigest(),
        }
        keygen_failure = subprocess.CompletedProcess([], 1, stdout="", stderr="keygen failed")

        with (
            mock.patch.object(ssh_transport, "_require_tool", side_effect=lambda name: name),
            mock.patch.object(
                ssh_transport,
                "_choose_network",
                return_value=ipaddress.IPv4Network("10.23.45.0/30"),
            ),
            mock.patch.object(
                ssh_transport,
                "_mac_pair",
                return_value=("02:00:00:00:00:01", "02:00:00:00:00:02"),
            ),
            mock.patch(
                "fplinux_cli.runtime.ssh_transport.subprocess.run", return_value=keygen_failure
            ),
            pytest.raises(SystemExit, match="ssh-keygen failed"),
        ):
            ssh_transport.prepare_session(
                image,
                descriptor,
                "phone",
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 1},
                runtime_root=ssh_runtime.root,
            )

        assert not ((current / "phone.json").exists())
        assert (list((ssh_runtime.root / "sessions").iterdir())) == ([])

    def test_pull_keeps_destination_when_mocked_remote_changes_during_download(
        self, ssh_runtime: SshRuntime
    ) -> None:
        """Keep the real local destination when the controlled remote boundary changes."""
        destination = ssh_runtime.directory / "download.bin"
        destination.write_bytes(b"old")
        expected_hash = hashlib.sha256(b"data").hexdigest()
        changed_hash = hashlib.sha256(b"next").hexdigest()

        def download(_session: object, command: str) -> subprocess.CompletedProcess[str]:
            _verb, _remote, local = shlex.split(command)
            Path(local).write_bytes(b"data")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with (
            mock.patch.object(ssh_transport, "_validate_session", side_effect=lambda value: value),
            mock.patch.object(
                ssh_transport,
                "_remote_metadata",
                side_effect=[(4, expected_hash), (4, changed_hash)],
            ),
            mock.patch.object(ssh_transport, "_sftp", side_effect=download),
            pytest.raises(SystemExit, match="changed while it was downloaded"),
        ):
            ssh_transport.pull({}, "/root/source.bin", str(destination))

        assert (destination.read_bytes()) == (b"old")
        assert (list(destination.parent.glob(f".{destination.name}.*"))) == ([])
