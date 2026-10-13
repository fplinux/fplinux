# SPDX-License-Identifier: GPL-2.0-only
"""Replace USB loading and the remote reader in a delivered archive."""

import shutil
import subprocess
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    import argparse
    from collections.abc import Callable
    from typing import BinaryIO, TextIO


def run_bundle(  # noqa: PLR0913 -- match the delivered runner's loader boundary.
    bundle: Path,
    runtime: dict[str, Any],
    options: argparse.Namespace,
    emit: Callable[[str], None],
    *,
    device_data: Path | None = None,
    output: TextIO | None = None,
    setup_host: bool = False,
    session_root: Path | None = None,
) -> tuple[Any, dict[str, str]]:
    """Supply controlled phone bytes, or fail at the host startup boundary."""
    del runtime, setup_host, options
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
    assert device_data is not None
    shutil.copyfile(device_data, record / f"{number}.cpio")
    for name in ("pinmap.bin", "keymap.bin"):
        source = device_data.parent / name
        if source.is_file():
            shutil.copyfile(source, record / f"{number}-{name}")
    print("private backend details", file=output)
    # Native loaders append through their own file handle to the same run log.
    assert output is not None
    log_path = max((bundle / "logs").glob("*.log"))
    with log_path.open("a", encoding="utf-8") as native_output:
        native_output.write("native loader details\n")
    emit("ram-loader-complete")
    emit("linux-start")
    emit("linux-transition")
    emit("ssh-ready")
    assert session_root is not None
    session_root.mkdir(parents=True, exist_ok=True)
    (session_root / "session.json").write_text("controlled ready session\n")
    (bundle / "phone-live").touch()
    return _phone_connection(bundle, record, session_root)


def reconnect_live_bundle(
    bundle: Path, runtime: dict[str, Any], *, session_root: Path
) -> tuple[Any, dict[str, str]] | None:
    """Control live-phone presence and authenticated reconnect failures without USB."""
    del runtime
    with (bundle / "phone-probes").open("a") as probes:
        probes.write("probe\n")
    failure = bundle / "phone-reconnect-failure"
    if failure.exists():
        raise SystemExit(failure.read_text().strip())
    if not (bundle / "phone-live").exists() or not (session_root / "session.json").exists():
        return None
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
                saved = Path((bundle / "phone-backup-path").read_text())
                with saved.open("rb") as source:
                    shutil.copyfileobj(source, destination)
                if progress is not None:
                    progress(destination.tell())
            else:
                raise AssertionError("Unexpected phone command: " + command)

    return Phone(), {"status": "ready"}
