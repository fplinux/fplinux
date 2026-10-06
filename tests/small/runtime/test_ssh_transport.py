# SPDX-License-Identifier: GPL-2.0-only
"""Small behavior tests for session-bound USB-NCM SSH transport."""

from __future__ import annotations

import contextlib
import hashlib
import io
import ipaddress
import itertools
import json
import shlex
import subprocess
from functools import partial
from pathlib import Path
from typing import Any
from unittest import mock

import pytest
from fplinux_cli.runtime import ssh_transport

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
        "boot_artifacts": {"required": []},
        "container_image_recipe": "7" * 64,
        "container_image_content": "4" * 64,
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


class SshTransportSmallTests:
    """Exercise in-process session, bundle and transfer boundaries."""

    @pytest.fixture(autouse=True)
    def _ssh_runtime_root(self, tmp_path: Path) -> None:
        """Create an isolated user runtime root and one synthetic bundle identity."""
        self.directory = tmp_path
        self.root = self.directory / "runtime"
        self.root.mkdir(mode=0o700)

    def _session(self, *, status: str = "ready") -> dict[str, Any]:
        return create_ready_session(self.root, status=status)

    def test_open_shell_rejects_a_noninteractive_process(self) -> None:
        """Do not launch forced-PTY SSH when the host has no input terminal."""
        session = self._session()
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch("fplinux_cli.runtime.ssh_transport.os.isatty", return_value=False),
            mock.patch("fplinux_cli.runtime.ssh_transport.os.execv") as execute,
            pytest.raises(SystemExit, match="interactive SSH requires a terminal"),
        ):
            ssh_transport.open_shell(session)

        execute.assert_not_called()

    def test_open_shell_executes_for_an_input_terminal(self) -> None:
        """Replace the runner with ssh forcing a remote PTY for the session's phone."""
        session = self._session()
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch("fplinux_cli.runtime.ssh_transport.os.isatty", return_value=True),
            mock.patch(
                "fplinux_cli.runtime.ssh_transport.shutil.which", return_value="/usr/bin/ssh"
            ),
            mock.patch("fplinux_cli.runtime.ssh_transport.os.execv") as execute,
        ):
            ssh_transport.open_shell(session)

        execute.assert_called_once()
        program, argv = execute.call_args.args
        assert (program) == ("/usr/bin/ssh")
        assert (argv[0]) == ("/usr/bin/ssh")
        assert ("-tt") in (argv)
        assert (argv[-1]) == ("root@10.23.45.2")

    def test_bundle_identity_rejects_a_runtime_image_outside_its_build_manifest(self) -> None:
        """Refuse reconnect state when the RAM payload no longer matches the generation."""
        bundle = self.directory / "bundle"
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
        self, changed: dict[str, str]
    ) -> None:
        """A named profile cannot reuse the default bundle's SSH identity."""
        bundle = self.directory / "bundle"
        runtime, _generation = write_bundle(bundle)

        with (
            pytest.raises(SystemExit, match="runtime target, profile or build type"),
        ):
            ssh_transport.bundle_identity(bundle, {**runtime, **changed})

    def test_current_session_loads_ready_bound_session(self) -> None:
        """A ready RAM session remains usable through its private binding."""
        state = self._session()
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)

        with mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root):
            loaded = ssh_transport.load_current_session("phone")

        assert (loaded) == (state)

    def test_device_identity_accepts_the_running_selected_kernel(self) -> None:
        """Accept an authenticated session when uname identifies the selected runtime."""
        session = self._session()
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

    def test_device_identity_rejects_a_different_running_kernel(self) -> None:
        """Reject a ready authenticated session running another device runtime."""
        session = self._session()
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

    def test_device_identity_rejects_a_failed_kernel_probe(self) -> None:
        """A matching-looking stdout cannot hide a failed authenticated probe."""
        session = self._session()
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

    @pytest.mark.parametrize(
        "rtc", [pytest.param("kept", id="kept"), pytest.param("written", id="written")]
    )
    def test_clock_sync_sends_host_utc_seconds_and_reports_the_phone_clock(self, rtc: str) -> None:
        """One phone command carries whole host UTC seconds; its RTC outcome is shown."""
        session = self._session()
        result = subprocess.CompletedProcess(
            [], 0, stdout=f"system=1788739201 rtc={rtc}\n", stderr=""
        )
        output = io.StringIO()
        with (
            mock.patch("fplinux_cli.runtime.ssh_transport.time.time", return_value=1788739200.75),
            mock.patch.object(ssh_transport, "run_remote", return_value=result) as remote,
            contextlib.redirect_stdout(output),
        ):
            ssh_transport.sync_clock(session)

        assert (remote.call_count) == (1)
        assert (remote.call_args.args[:2]) == ((session, "fplinux-clock 1788739200"))
        assert (output.getvalue()) == (f"Phone clock set to 2026-09-07T00:00:01Z (RTC {rtc}).\n")

    @pytest.mark.parametrize(
        ("result", "warning"),
        [
            pytest.param(
                subprocess.CompletedProcess(
                    [],
                    1,
                    stdout="system=1788739200 rtc=failed\n",
                    stderr="fplinux-clock: cannot store a readable time in /dev/rtc0\n",
                ),
                "fplinux ssh: phone clock set to 2026-09-07T00:00:00Z, but its RTC was not "
                "written: fplinux-clock: cannot store a readable time in /dev/rtc0\n",
                id="rtc-not-written",
            ),
            pytest.param(
                subprocess.CompletedProcess(
                    [], 255, stdout="", stderr="Connection closed by 10.23.45.2 port 22\n"
                ),
                "fplinux ssh: phone clock was not set: Connection closed by 10.23.45.2 port 22\n",
                id="transport-lost",
            ),
            pytest.param(
                subprocess.CompletedProcess([], 0, stdout="done\n", stderr=""),
                "fplinux ssh: phone clock was not set: unexpected clock status (exit 0)\n",
                id="unexpected-status",
            ),
        ],
    )
    def test_clock_sync_failure_warns_and_returns(
        self, result: subprocess.CompletedProcess[str], warning: str
    ) -> None:
        """A clock failure is a warning, so the caller can still report a ready session."""
        session = self._session()
        output = io.StringIO()
        errors = io.StringIO()
        with (
            mock.patch("fplinux_cli.runtime.ssh_transport.time.time", return_value=1788739200.0),
            mock.patch.object(ssh_transport, "run_remote", return_value=result),
            contextlib.redirect_stdout(output),
            contextlib.redirect_stderr(errors),
        ):
            ssh_transport.sync_clock(session)

        assert (output.getvalue()) == ("")
        assert (errors.getvalue()) == (warning)

    def test_current_session_rejects_an_unknown_field(self) -> None:
        """An unrecognized host-state record is not a reconnect session."""
        state = {**self._session(), "unexpected": "value"}
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            pytest.raises(SystemExit, match="unexpected fields"),
        ):
            ssh_transport.load_current_session("phone")

    def test_usb_session_text_is_exactly_seven_fields_with_nul_padding(self) -> None:
        """Emit the canonical fixed USB-gadget consumer record."""
        config = ssh_transport._usb_config(  # noqa: SLF001
            "0123456789abcdef0123456789abcdef",
            ipaddress.IPv4Network("10.23.45.0/30"),
            "02:00:00:00:00:01",
            "02:00:00:00:00:02",
        )
        expected = (
            "usb_serial=0123456789abcdef0123456789abcdef\n"
            "phone_address=10.23.45.2\n"
            "host_address=10.23.45.1\n"
            "netmask=255.255.255.252\n"
            "broadcast=10.23.45.3\n"
            "device_mac=02:00:00:00:00:02\n"
            "host_mac=02:00:00:00:00:01\n"
        ).encode("ascii")

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

    def test_reacquire_retries_mocked_usb_ncm_and_ssh_boundaries(self) -> None:
        """Retry transient failures reported by controlled USB, NCM, and SSH boundaries."""
        state = self._session()
        failures = [
            subprocess.CompletedProcess([], 255, stdout="", stderr="not ready"),
            subprocess.CompletedProcess([], 0, stdout=f"{state['session_id']}\n", stderr=""),
        ]
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
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

    def test_fresh_session_reports_usb_once_before_network_and_ssh_are_ready(self) -> None:
        """A matched USB device is observable even while network and SSH retry."""
        state = self._session()
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
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
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
        self, devices: list[Path], diagnostic: str
    ) -> None:
        """Only one USB device matching the selected session can report arrival."""
        state = {**self._session(), "wait_seconds": 1}
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

    def test_failed_current_config_publication_removes_the_ready_pointer_and_session(self) -> None:
        """Never leave a direct config pointing at an incomplete ready session."""
        session = self._session()
        write_private_text = ssh_transport._write_private_text  # noqa: SLF001

        def fail_config(path: Path, value: str) -> None:
            if path.name == "phone.ssh-config":
                message = "disk full"
                raise OSError(message)
            write_private_text(path, value)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.object(ssh_transport, "_write_private_text", side_effect=fail_config),
            pytest.raises(OSError, match="disk full"),
        ):
            ssh_transport._mark_current(session)  # noqa: SLF001

        current = self.root / "current"
        assert not ((current / "phone.json").exists())
        assert not ((current / "phone.ssh-config").exists())
        assert (list((self.root / "sessions").iterdir())) == ([])

    def test_failed_prepare_erases_keys_and_invalidates_prior_current(self) -> None:
        """A failed session preparation leaves no usable pointer or prepared key directory."""
        prior = self._session()
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(prior), encoding="utf-8")
        image = self.directory / "ramboot.bin"
        image.write_bytes(b"DHTB" + b"\0" * 1532)
        descriptor = {
            "offset": 1024,
            "bytes": ssh_transport.SESSION_BYTES,
            "template_sha256": hashlib.sha256(b"\0" * 512).hexdigest(),
        }
        keygen_failure = subprocess.CompletedProcess([], 1, stdout="", stderr="keygen failed")

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
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
            )

        assert not ((current / "phone.json").exists())
        assert (list((self.root / "sessions").iterdir())) == ([])

    def test_pull_keeps_destination_when_mocked_remote_changes_during_download(self) -> None:
        """Keep the real local destination when the controlled remote boundary changes."""
        destination = self.directory / "download.bin"
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
