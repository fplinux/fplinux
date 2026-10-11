# SPDX-License-Identifier: GPL-2.0-only
"""Replace USB loading and the remote reader in a delivered archive."""

import shutil
import subprocess
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
    del runtime, setup_host, session_root
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
    reconnect = options.reconnect
    number = len(list(record.glob("*.cpio"))) + 1
    if not reconnect:
        assert device_data is not None
        shutil.copyfile(device_data, record / f"{number}.cpio")
        for name in ("pinmap.bin", "keymap.bin"):
            source = device_data.parent / name
            if source.is_file():
                shutil.copyfile(source, record / f"{number}-{name}")
    emit("host-preparation")
    emit("network-preparation")
    emit("session-preparation")
    emit("payload-preparation")
    emit("waiting-for-device")
    emit("device-found")
    emit("ram-loader-start")
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

    class Phone:
        """Read controlled geometry and a real test-owned raw backup."""

        def run_shell(self, _session: dict[str, Any]) -> int:
            """Replace only the interactive SSH executable boundary."""
            with (record / "shell-opened").open("a") as shell:
                shell.write("opened\n")
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
            with (record / "reader-commands").open("a") as commands:
                commands.write(command + "\n")
            if command.startswith("cat "):
                destination.write(
                    b"id_bytes=e521\nchip=controlled chip\n"
                    b"page_main_bytes=2048\noob_bytes=64\npages_per_block=64\n"
                    b"block_count=1024\nraw_bytes=138412032\ngeometry_source=table\n"
                    b"feature_a0=00\nfeature_b0=00\nfeature_c0=00\n"
                )
            elif command.startswith("exec dd if=/dev/raw-nand "):
                saved = Path((bundle / "phone-backup-path").read_text())
                with saved.open("rb") as source:
                    shutil.copyfileobj(source, destination)
                if progress is not None:
                    progress(destination.tell())
            else:
                raise AssertionError("Unexpected phone command: " + command)

    return Phone(), {"status": "ready"}
