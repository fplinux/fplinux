# SPDX-License-Identifier: GPL-2.0-only
"""Exercise final native launchers through initialization without a connected phone."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import time
import uuid
import zipfile
from pathlib import Path, PurePosixPath

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.packaging.distribution import distribution_matrix, distribution_records


def _require_ordinary_account(host: str) -> None:
    if host == "windows":
        if os.name != "nt":
            fail("Windows archive smoke requires native Windows")
        groups = subprocess.run(
            ["whoami", "/groups"],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        if groups.returncode or "S-1-5-32-544" in groups.stdout:
            fail("Windows archive smoke requires an ordinary non-administrator account")
    elif os.name != "posix" or os.geteuid() == 0:
        fail("Linux archive smoke requires an ordinary Linux account")


def _extract_archive(archive: Path, destination: Path) -> None:
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            prefix = f"{archive.stem}/"
            relative = item.filename.removeprefix(prefix)
            name = PurePosixPath(relative)
            if (
                not item.filename.startswith(prefix)
                or name.is_absolute()
                or ".." in name.parts
                or item.is_dir()
            ):
                fail(f"invalid runnable archive member: {item.filename}")
            path = destination.joinpath(*name.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source.read(item))
            path.chmod((item.external_attr >> 16) & 0o777)


def _native_python(root: Path, host: str) -> list[str]:
    if host == "windows":
        return [str(root / "runtime/python.exe")]
    return [
        str(root / "runtime/lib/ld-musl-x86_64.so.1"),
        "--library-path",
        str(root / "runtime/lib"),
        str(root / "runtime/bin/python3.14"),
    ]


def _stop_launcher(process: subprocess.Popen[bytes], host: str) -> None:
    if process.poll() is not None:
        return
    if host == "windows":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            check=False,
            timeout=30,
        )
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)
        fail("native launcher did not stop after the no-phone smoke")


def _launch_to_invitation(root: Path, host: str, *, attempt: int) -> None:
    events = root / f"smoke-events-{attempt}.jsonl"
    output = root / f"smoke-output-{attempt}.txt"
    if host == "windows":
        command = ["cmd.exe", "/d", "/c", str(root / "Run.cmd"), "--events", str(events)]
    else:
        command = ["sh", str(root / "Run.sh"), "--events", str(events)]
    environment = os.environ.copy()
    environment.update(TMP=str(root / "temp"), TEMP=str(root / "temp"))
    creation_flags = 0
    if sys.platform == "win32":
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP
    failure = None
    with output.open("wb") as stream:
        process = subprocess.Popen(
            command,
            cwd=root,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=stream,
            stderr=subprocess.STDOUT,
            start_new_session=host == "linux",
            creationflags=creation_flags,
        )
        try:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                records = events.read_text().splitlines() if events.is_file() else []
                names = [json.loads(line)["event"] for line in records if line.endswith("}")]
                if "device-found" in names:
                    fail("native archive smoke found a phone; disconnect it before this check")
                if "waiting-for-device" in names:
                    break
                if process.poll() is not None:
                    failure = "native launcher exited before its invitation"
                    break
                time.sleep(0.1)
            else:
                failure = "native launcher did not reach its no-phone invitation"
        finally:
            _stop_launcher(process, host)
    if failure is not None:
        diagnostic = output.read_text(errors="replace").strip()
        if diagnostic:
            failure = f"{failure}:\n{diagnostic[-4096:]}"
        fail(failure)
    if len(list((root / "logs").glob("*/runtime.log"))) != attempt:
        fail("native launcher did not create a separate runtime log on each launch")


def _smoke_archive(archive: Path, workspace: Path, host: str) -> None:
    root = workspace / archive.stem
    root.mkdir()
    _extract_archive(archive, root)
    runtime = json.loads((root / "runtime-manifest.json").read_bytes())
    storage_paths = ["data", "logs", "data/session", "temp"]
    if runtime["device_data"] is not None:
        storage_paths.append("data/reader/session")
    for relative in storage_paths:
        (root / relative).mkdir(parents=True, exist_ok=True)
    storage_probe = Path(__file__).with_name("smoke") / "storage.py"
    for attempt in (1, 2):
        _launch_to_invitation(root, host, attempt=attempt)
        result = subprocess.run(
            [
                *_native_python(root, host),
                "-B",
                "-I",
                str(storage_probe),
                str(root),
                *storage_paths,
            ],
            cwd=root,
            check=False,
            timeout=60,
        )
        if result.returncode:
            fail(f"native storage creation/reopening failed: {archive.name}")


def smoke_distribution(directory: Path, *, host: str) -> None:
    """Run every final archive for one host twice under an ordinary account."""
    _require_ordinary_account(host)
    directory = directory.resolve()
    sources = list(directory.glob("FPLinux-*-sources.zip"))
    if len(sources) != 1:
        fail("native smoke requires one final corresponding-sources archive")
    with zipfile.ZipFile(sources[0]) as archive:
        records = distribution_records(archive.read(f"{sources[0].stem}/DISTRIBUTION.json"))
    expected = set(distribution_matrix())
    actual = {(record["target"], record["host"]) for record in records}
    if actual != expected or len(records) != len(expected):
        fail("native smoke requires every supported target/host exactly once")
    workspace = directory / f"fplinux-native-smoke-{uuid.uuid4().hex}"
    # TemporaryDirectory uses mode 0700, which replaces inherited Windows permissions.
    workspace.mkdir()
    try:
        for record in records:
            if record["host"] != host:
                continue
            name = common.relative_name(record["archive"], field="runtime archive")
            if len(PurePosixPath(name).parts) != 1:
                fail("native smoke runtime archive must be a basename")
            path = directory / name
            if not path.is_file() or common.sha256_file(path) != record["sha256"]:
                fail(f"final runtime archive hash differs: {name}")
            print(f"Native {host} smoke: {name}", flush=True)
            _smoke_archive(path, workspace, host)
    finally:
        shutil.rmtree(workspace)
