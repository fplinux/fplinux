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
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.data_paths import DATA_ENV, data_root
from fplinux_cli.packaging.distribution import distribution_records
from fplinux_cli.runtime.release_catalog import (
    load_catalog,
    read_target_manifest,
    target_runtime_root,
)

if TYPE_CHECKING:
    from fplinux_cli.runtime.release_catalog import ReleaseCatalog


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
                or name.as_posix() != relative
                or "\\" in relative
                or ":" in relative
                or item.is_dir()
            ):
                fail(f"invalid runnable archive member: {item.filename}")
            path = destination.joinpath(*name.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            with source.open(item) as member, path.open("xb") as output:
                shutil.copyfileobj(member, output)
            path.chmod((item.external_attr >> 16) & 0o777)


def _native_launcher(host: str) -> list[str]:
    if host == "windows":
        return ["cmd.exe", "/d", "/c", "Run.cmd"]
    return ["sh", "Run.sh"]


def _native_python(root: Path, host: str) -> list[str]:
    if host == "windows":
        return [str(root / "runtime/python.exe")]
    return [
        str(root / "runtime/lib/ld-musl-x86_64.so.1"),
        "--library-path",
        str(root / "runtime/lib"),
        str(root / "runtime/bin/python3.15"),
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


def _smoke_environment(root: Path) -> dict[str, str]:
    user_data = str(root.parent / "user-data")
    return {
        **os.environ,
        DATA_ENV: str(data_root(root, environment={})),
        "LOCALAPPDATA": user_data,
        "XDG_DATA_HOME": user_data,
    }


def _check_menu(root: Path, host: str, catalog: ReleaseCatalog) -> None:
    result = subprocess.run(
        _native_launcher(host),
        cwd=root,
        env=_smoke_environment(root),
        input=b"0\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=60,
    )
    menu = result.stdout.decode(errors="replace")
    if result.returncode or any(f"({target})" not in menu for target in catalog.targets):
        fail(f"native launcher menu did not list every target and exit cleanly:\n{menu}")


def _launch_to_invitation(  # noqa: PLR0913 -- host, model, action and attempt identify the log.
    root: Path,
    host: str,
    scope: Path,
    target: str,
    *,
    attempt: int,
    action: tuple[str, ...] = ("run",),
) -> None:
    events = scope / f"smoke-events-{attempt}.jsonl"
    output = scope / f"smoke-output-{attempt}.txt"
    target_at = 2 if action[0] == "card" else 1
    operation = [*action[:target_at], target, *action[target_at:]]
    command = [*_native_launcher(host), *operation, "--events", str(events)]
    environment = _smoke_environment(root)
    environment.update(TMP=str(scope / "temp"), TEMP=str(scope / "temp"))
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
                lines = events.read_text(encoding="utf-8").splitlines() if events.is_file() else []
                records = [json.loads(line) for line in lines if line.endswith("}")]
                names = {record["event"] for record in records}
                if "device-found" in names:
                    fail("native archive smoke found a phone; disconnect it before this check")
                if any(record["target"] != target for record in records):
                    fail(f"native launcher reported another target instead of {target}")
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
    if len(list((scope / "logs").glob("*/runtime.log"))) != attempt:
        fail("native launcher did not create a separate runtime log on each launch")


def _smoke_target(root: Path, host: str, catalog: ReleaseCatalog, target: str) -> None:
    runtime = read_target_manifest(root, catalog, target)
    scope = target_runtime_root(data_root(root, environment={}), root, target)
    storage_paths = [scope, scope / "logs", scope / "session", scope / "temp"]
    if runtime["device_data"] is not None:
        storage_paths.append(scope / "reader/session")
    for path in storage_paths:
        path.mkdir(parents=True, exist_ok=True)
    storage_probe = Path(__file__).with_name("smoke") / "storage.py"
    for attempt in (1, 2):
        _launch_to_invitation(root, host, scope, target, attempt=attempt)
        result = subprocess.run(
            [
                *_native_python(root, host),
                "-B",
                "-I",
                str(storage_probe),
                str(root),
                *(str(path) for path in storage_paths),
            ],
            cwd=root,
            env=_smoke_environment(root),
            check=False,
            timeout=60,
        )
        if result.returncode:
            fail(f"native storage creation/reopening failed: {root.name}: {target}")
    if "card/layout.json" in catalog.targets[target].files:
        _launch_to_invitation(root, host, scope, target, attempt=3, action=("card", "inspect"))
        _launch_to_invitation(
            root, host, scope, target, attempt=4, action=("run", "--boot", "microsd")
        )


def _smoke_archive(archive: Path, workspace: Path, host: str, targets: set[str]) -> None:
    root = workspace / archive.stem
    root.mkdir()
    _extract_archive(archive, root)
    catalog = load_catalog(root)
    if set(catalog.targets) != targets:
        fail(f"final runtime targets differ from the source distribution: {archive.name}")
    _check_menu(root, host, catalog)
    for target in sorted(targets):
        print(f"Native {host} smoke: {archive.name}: {target}", flush=True)
        _smoke_target(root, host, catalog, target)


def smoke_distribution(directory: Path, *, host: str) -> None:
    """Check the native selector and run each target twice under an ordinary account."""
    _require_ordinary_account(host)
    directory = directory.resolve()
    sources = list(directory.glob("FPLinux-*-sources.zip"))
    if len(sources) != 1:
        fail("native smoke requires one final corresponding-sources archive")
    with zipfile.ZipFile(sources[0]) as archive:
        records = distribution_records(archive.read(f"{sources[0].stem}/DISTRIBUTION.json"))
    selected = [record for record in records if record["host"] == host]
    archives = {(record["archive"], record["sha256"]) for record in selected}
    if len(archives) != 1:
        fail(f"native smoke requires one final runtime archive for {host}")
    name, digest = archives.pop()
    path = directory / name
    if path.name != name or not path.is_file() or common.sha256_file(path) != digest:
        fail(f"final runtime archive hash differs: {name}")
    targets = {record["target"] for record in selected}
    workspace = directory / f"fplinux-native-smoke-{uuid.uuid4().hex}"
    # TemporaryDirectory uses mode 0700, which replaces inherited Windows permissions.
    workspace.mkdir()
    try:
        _smoke_archive(path, workspace, host, targets)
    finally:
        shutil.rmtree(workspace)
