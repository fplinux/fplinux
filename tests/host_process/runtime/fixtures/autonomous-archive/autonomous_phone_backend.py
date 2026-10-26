# SPDX-License-Identifier: GPL-2.0-only
"""Replace USB loading and the remote reader in a delivered archive."""

import hashlib
import json
import shlex
import shutil
import subprocess
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import BinaryIO, TextIO

    from fplinux_cli.runtime.bundle import ValidatedBundle


def run_bundle(  # noqa: PLR0913 -- match the delivered runner's loader boundary.
    admitted: ValidatedBundle,
    emit: Callable[[str], None],
    *,
    device_data: Path | None = None,
    output: TextIO | None = None,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    loader_trace: bool = False,
    log: Path | None = None,
) -> tuple[Any, dict[str, str]]:
    """Supply controlled phone bytes, or fail at the host startup boundary."""
    del loader_trace
    bundle, runtime = admitted.root, admitted.runtime
    failure = bundle / "phone-startup-failure"
    if failure.is_file():
        emit("host-preparation")
        raise SystemExit(failure.read_text(encoding="utf-8").strip())
    if (bundle / "phone-startup-timeout").is_file():
        emit("host-preparation")
        raise subprocess.TimeoutExpired(
            ["powershell.exe", "-EncodedCommand", "private-command-payload"], 20
        )
    if (bundle / "phone-startup-cancel").is_file():
        emit("waiting-for-device")
        raise SystemExit(130)
    record = bundle / "data" / "loaded"
    record.mkdir(exist_ok=True)
    number = len(list(record.glob("*.cpio"))) + 1
    emit("host-preparation")
    emit("network-preparation")
    emit("session-preparation")
    emit("payload-preparation")
    emit("waiting-for-device")
    while (bundle / "phone-hold-loader").exists():
        (record / "loader-waiting").touch()
        time.sleep(0.02)
    emit("device-found")
    emit("ram-loader-start")
    if runtime["identity"]["platform"]["name"] == "sc9820e":
        assert device_data is None
        with (record / "ram-loads").open("a") as loads:
            loads.write("loaded\n")
    else:
        assert device_data is not None
        assert prepared_data_sha256 == hashlib.sha256(device_data.read_bytes()).hexdigest()
        shutil.copyfile(device_data, record / f"{number}.cpio")
    print("private backend details", file=output)
    # Native loaders append through their own file handle to the same run log.
    assert output is not None
    assert log is not None
    log_path = log
    with log_path.open("a", encoding="utf-8") as native_output:
        native_output.write("native loader details\n")
    emit("ram-loader-complete")
    emit("linux-start")
    emit("linux-transition")
    emit("ssh-ready")
    session_root.mkdir(parents=True, exist_ok=True)
    (session_root / "session.json").write_text(
        json.dumps({"prepared_data_sha256": prepared_data_sha256})
    )
    (bundle / "phone-live").touch()
    return _phone_connection(bundle, record, session_root)


def reconnect_live_bundle(
    admitted: ValidatedBundle,
    *,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    required: bool = False,
) -> tuple[Any, dict[str, str]] | None:
    """Control live-phone presence and authenticated reconnect failures without USB."""
    bundle = admitted.root
    with (bundle / "phone-probes").open("a") as probes:
        probes.write("probe\n")
    failure = bundle / "phone-reconnect-failure"
    if failure.exists():
        raise SystemExit(failure.read_text().strip())
    if not (bundle / "phone-live").exists() or not (session_root / "session.json").exists():
        if required:
            message = "No ready normal phone session is available"
            raise SystemExit(message)
        return None
    state = json.loads((session_root / "session.json").read_text())
    if state["prepared_data_sha256"] != prepared_data_sha256:
        message = "The running phone uses different prepared phone data"
        raise SystemExit(message)
    return _phone_connection(bundle, bundle / "data/loaded", session_root)


def _phone_connection(
    bundle: Path, record: Path, session_root: Path
) -> tuple[Any, dict[str, str]]:
    class Phone:
        """Read controlled geometry and a real test-owned raw backup."""

        def run_shell(self, _session: dict[str, Any]) -> int:
            """Replace only the interactive SSH executable boundary."""
            with (record / "shell-opened").open("a") as shell:
                shell.write("opened\n")
            while (bundle / "phone-hold-shell").exists():
                time.sleep(0.02)
            print("interactive shell output")
            return 0

        def run_remote(
            self,
            _session: dict[str, Any],
            command: str,
            *,
            capture_output: bool = False,
            shared: bool = False,
        ) -> subprocess.CompletedProcess[str]:
            assert capture_output
            assert shared
            if command.startswith("fplinux-clock "):
                return subprocess.CompletedProcess(command, 0, "system=1234 rtc=kept\n", "")
            if command.startswith("/usr/libexec/fplinux/preparation "):
                assert (session_root / "session.json").exists()
                arguments = shlex.split(command)[1:]
                assert arguments[-1] == "/run/fplinux-preparation-status"
                with (record / "preparation-stages").open("a") as stages:
                    stages.write(
                        json.dumps(
                            {
                                "arguments": arguments,
                                "phone_data": Path(
                                    (bundle / "phone-prepared-path").read_text(encoding="utf-8")
                                ).exists(),
                                "storage_checked": (record / "storage-checks").exists(),
                            }
                        )
                        + "\n"
                    )
                failure = bundle / "phone-progress-failure"
                if failure.exists():
                    mode = failure.read_text().strip()
                    if mode == "error-stage" and arguments[1] != "error":
                        return subprocess.CompletedProcess(command, 0, "", "")
                    if mode in {"io-error", "error-stage"}:
                        message = "phone progress connection unavailable"
                        raise OSError(message)
                    return subprocess.CompletedProcess(command, 127, "", "helper unavailable\n")
                return subprocess.CompletedProcess(command, 0, "", "")
            assert "FPLINUX_SWAPS" in command
            assert (session_root / "session.json").exists()
            with (record / "storage-checks").open("a") as checks:
                checks.write("reader keys present\n")
            mounts = (
                "rootfs / rootfs rw 0 0\n"
                "/dev/loop0 /run/fplinux-root/lower squashfs ro,relatime 0 0\n"
            )
            swaps = "Filename Type Size Used Priority\n/dev/zram0 partition 65532 0 -2\n"
            if (bundle / "phone-storage-mounted").exists():
                mounts += "/dev/mmcblk0p1 /media/mmc ext4 rw 0 0\n"
            if (bundle / "phone-storage-swap").exists():
                swaps += "/dev/mmcblk0p2 partition 1048572 0 -3\n"
            if (bundle / "phone-storage-loop").exists():
                mounts += "/dev/loop1 /media/card-image squashfs ro,relatime 0 0\n"
            return subprocess.CompletedProcess(
                command, 0, mounts + "\nFPLINUX_SWAPS\n" + swaps, ""
            )

        def stream_remote(
            self,
            session: dict[str, Any],
            command: str,
            destination: BinaryIO,
            *,
            timeout: float,
            progress: Callable[[int], None] | None = None,
        ) -> None:
            """Keep the requested reader command and deliver its fixture bytes."""
            del session, timeout
            assert (session_root / "session.json").exists()
            with (record / "reader-commands").open("a") as commands:
                commands.write(command + "\n")
            if command.startswith("cat "):
                destination.write((bundle / "phone-geometry.txt").read_bytes())
            elif command.startswith("exec dd if=/dev/raw-nand "):
                while (bundle / "phone-hold-read").exists():
                    (record / "reader-waiting").touch()
                    time.sleep(0.02)
                if (bundle / "phone-read-failure").exists():
                    message = "controlled NAND read failure"
                    raise OSError(message)
                saved = Path((bundle / "phone-backup-path").read_text())
                with saved.open("rb") as source:
                    if (bundle / "phone-measured-progress").exists():
                        destination.write(source.read(69206016))
                        time.sleep(1.05)
                        if progress is not None:
                            progress(destination.tell())
                    shutil.copyfileobj(source, destination)
                if progress is not None:
                    progress(destination.tell())
            else:
                raise AssertionError("Unexpected phone command: " + command)

    return Phone(), {"status": "ready"}
