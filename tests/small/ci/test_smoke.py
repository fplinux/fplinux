# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native smoke extraction and launcher processes without a phone."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
from fplinux_cli.ci import smoke
from fplinux_cli.device_data.store import locked_phone_store
from fplinux_cli.runtime.data_paths import archive_runtime_root

from tests.distribution_support import (
    RELEASE_ID,
    SELECTIONS,
    json_bytes,
    release_fields,
    write_zip,
)
from tests.process import python_environment

_PREPARATION_TARGET = "nokia-ta1618"


def _bundle_recipe(
    files: dict[str, bytes], target: str, *, prepare_device_data: bool
) -> dict[str, object]:
    runtime = json_bytes(
        {
            "release_id": RELEASE_ID,
            "target": target,
            "device_data": {} if prepare_device_data else None,
        }
    )
    digest = hashlib.sha256(runtime).hexdigest()
    manifest = json_bytes(
        {
            "release_id": RELEASE_ID,
            "target": target,
            "files": {
                "runtime-manifest.json": {"mode": 0o644, "size": len(runtime), "sha256": digest}
            },
        }
    )
    sums = (
        f"{hashlib.sha256(manifest).hexdigest()}  build-manifest.json\n"
        f"{digest}  runtime-manifest.json\n"
    ).encode()
    references = {}
    for name, contents in (
        ("build-manifest.json", manifest),
        ("runtime-manifest.json", runtime),
        ("SHA256SUMS", sums),
    ):
        member = f"objects/{hashlib.sha256(contents).hexdigest()}"
        files[member] = contents
        references[name] = [member]
    return {"name": f"Test {target}", "files": references, "executables": []}


def _write_distribution(directory: Path, files: dict[str, bytes]) -> Path:
    directory.mkdir()
    records: list[dict[str, str]] = []
    for host in ("linux", "windows"):
        runtime = directory / f"FPLinux-{host}-x64-{RELEASE_ID}.zip"
        archive_files = dict(files)
        target_ids = [target for target, selected_host in SELECTIONS if selected_host == host]
        targets = {
            target: _bundle_recipe(
                archive_files, target, prepare_device_data=target == _PREPARATION_TARGET
            )
            for target in target_ids
        }
        archive_files["targets.json"] = json_bytes({"release_id": RELEASE_ID, "targets": targets})
        archive_files["menu.txt"] = "".join(
            f"  {number}. Test {target} ({target})\n"
            for number, target in enumerate(target_ids, start=1)
        ).encode()
        write_zip(runtime, archive_files, {"Run.sh"} & archive_files.keys())
        digest = hashlib.sha256(runtime.read_bytes()).hexdigest()
        records.extend(
            {
                "archive": runtime.name,
                "target": target,
                "host": host,
                "identity": "f" * 64,
                "sha256": digest,
            }
            for target in target_ids
        )
    sources = directory / f"FPLinux-{RELEASE_ID}-sources.zip"
    write_zip(
        sources,
        {"DISTRIBUTION.json": json_bytes({**release_fields(), "runtimes": records})},
        set(),
    )
    return directory


@pytest.fixture
def distribution(tmp_path: Path) -> Path:
    return _write_distribution(
        tmp_path / "distribution", {"README.txt": b"native smoke archive\n"}
    )


@pytest.mark.parametrize("storage_status", [0, 1], ids=["success", "storage-failure"])
def test_native_smoke_extracts_under_prepared_permissions_and_cleans_up(
    distribution: Path, monkeypatch: pytest.MonkeyPatch, storage_status: int
) -> None:
    original_files = set(distribution.iterdir())
    launched_roots: list[Path] = []

    def launch(root: Path, host: str, scope: Path, target: str, *, attempt: int) -> None:
        assert host == "windows"
        assert attempt in (1, 2)
        assert (root / "README.txt").read_bytes() == b"native smoke archive\n"
        assert scope == archive_runtime_root(root / ".data", root) / "targets" / target
        for relative in (".", "logs", "session", "temp"):
            assert (scope / relative).is_dir()
        assert (scope / "reader/session").is_dir() == (target == _PREPARATION_TARGET)
        launched_roots.append(root)

    def run_storage(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        root = kwargs["cwd"]
        assert isinstance(root, Path)
        environment = kwargs["env"]
        assert isinstance(environment, dict)
        assert Path(environment["LOCALAPPDATA"]).is_relative_to(root.parent)
        assert Path(environment["XDG_DATA_HOME"]).is_relative_to(root.parent)
        if command == ["cmd.exe", "/d", "/c", "Run.cmd"]:
            assert kwargs["input"] == b"0\n"
            return subprocess.CompletedProcess(command, 0, stdout=(root / "menu.txt").read_bytes())
        assert root == launched_roots[-1]
        return subprocess.CompletedProcess(command, storage_status)

    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)
    monkeypatch.setattr(smoke, "_launch_to_invitation", launch)
    monkeypatch.setattr(subprocess, "run", run_storage)

    if storage_status:
        with pytest.raises(SystemExit, match="native storage creation/reopening failed"):
            smoke.smoke_distribution(distribution, host="windows")
    else:
        smoke.smoke_distribution(distribution, host="windows")

    windows_targets = sum(host == "windows" for _target, host in SELECTIONS)
    expected_launches = 1 if storage_status else 2 * windows_targets
    assert len(launched_roots) == expected_launches
    assert len(set(launched_roots)) == 1
    assert all(root.parent.parent == distribution for root in launched_roots)
    assert all(not root.exists() for root in launched_roots)
    assert set(distribution.iterdir()) == original_files


@pytest.mark.skipif(os.name != "posix", reason="the launcher peer uses POSIX shell")
def test_native_smoke_reaches_two_invitations_with_personal_phone_store_locked(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fixtures = Path(__file__).parent / "fixtures/native-smoke"
    directory = _write_distribution(
        tmp_path / "distribution",
        {
            "Run.sh": (fixtures / "launcher.sh").read_bytes(),
            "phone-store.py": (fixtures / "phone-store.py").read_bytes(),
        },
    )
    original_files = set(directory.iterdir())
    storage_visits: dict[Path, int] = {}

    def run_storage(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        root = kwargs["cwd"]
        assert isinstance(root, Path)
        if command == ["sh", "Run.sh"]:
            menu_input = kwargs["input"]
            assert isinstance(menu_input, bytes)
            environment = kwargs["env"]
            assert isinstance(environment, dict)
            return run_process(
                command,
                cwd=root,
                env=environment,
                input=menu_input,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=5,
            )
        scope = Path(command[command.index(str(root)) + 1])
        assert scope.parent == archive_runtime_root(root / ".data", root) / "targets"
        target = scope.name
        storage_paths: tuple[str, ...] = (".", "logs", "session", "temp")
        if target == _PREPARATION_TARGET:
            storage_paths = (*storage_paths, "reader/session")
            markers = list(root.parent.rglob("smoke-marker.txt"))
            assert len(markers) == 1
            assert markers[0].read_bytes() == b"launcher phone store\n"
        for relative in storage_paths:
            assert (scope / relative).is_dir()
        assert (scope / "reader").exists() == ("reader/session" in storage_paths)
        assert tuple(command[command.index(str(root)) + 1 :]) == tuple(
            str(scope / relative) for relative in storage_paths
        )
        attempt = storage_visits.get(scope, 0) + 1
        storage_visits[scope] = attempt
        assert (
            sorted(path.parent.name for path in (scope / "logs").glob("*/runtime.log"))
            == [
                "smoke-events-1",
                "smoke-events-2",
            ][:attempt]
        )
        for index in range(1, attempt + 1):
            event = json.loads((scope / f"smoke-events-{index}.jsonl").read_bytes())
            assert event == {"event": "waiting-for-device", "target": target}
            log_directory = scope / f"logs/smoke-events-{index}"
            assert (log_directory / "runtime.log").read_bytes() == b"launcher initialized\n"
            assert (log_directory / "stopped").read_bytes() == b"stopped\n"
        return subprocess.CompletedProcess(command, 0)

    run_process = subprocess.run
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "personal-linux"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "personal-windows"))
    monkeypatch.setenv("PYTHONPATH", python_environment()["PYTHONPATH"])
    monkeypatch.setenv("FPLINUX_TEST_PYTHON", sys.executable)
    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)
    monkeypatch.setattr(subprocess, "run", run_storage)

    with locked_phone_store(_PREPARATION_TARGET) as personal_store:
        marker = personal_store / "smoke-marker.txt"
        marker.write_bytes(b"personal store sentinel\n")
        lock = personal_store / ".store.lock"
        lock.write_bytes(b"personal lock sentinel\n")
        lock_stat = lock.stat()
        personal_store.chmod(0o750)

        smoke.smoke_distribution(Path("distribution"), host="linux")

        assert marker.read_bytes() == b"personal store sentinel\n"
        assert lock.read_bytes() == b"personal lock sentinel\n"
        assert lock.stat().st_ino == lock_stat.st_ino
        assert lock.stat().st_mtime_ns == lock_stat.st_mtime_ns
        assert personal_store.stat().st_mode & 0o777 == 0o750

    assert {scope.name: attempts for scope, attempts in storage_visits.items()} == {
        target: 2 for target, host in SELECTIONS if host == "linux"
    }
    assert len({scope.parent for scope in storage_visits}) == 1
    assert all(not scope.exists() for scope in storage_visits)
    assert set(directory.iterdir()) == original_files


@pytest.mark.skipif(os.name != "posix", reason="the launcher peer uses POSIX shell")
def test_native_smoke_preserves_launcher_stderr_after_workspace_cleanup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    launcher = Path(__file__).parent / "fixtures/native-smoke/failing-launcher.sh"
    directory = _write_distribution(tmp_path / "distribution", {"Run.sh": launcher.read_bytes()})
    original_files = set(directory.iterdir())
    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)

    with pytest.raises(
        SystemExit, match="native launcher exited before its invitation"
    ) as failure:
        smoke.smoke_distribution(directory, host="linux")

    assert "fixture launcher cannot initialize" in str(failure.value)
    assert set(directory.iterdir()) == original_files
