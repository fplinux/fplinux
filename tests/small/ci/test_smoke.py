# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native smoke extraction and launcher processes without a phone."""

from __future__ import annotations

import hashlib
import os
import subprocess
from pathlib import Path

import pytest
from fplinux_cli.ci import smoke
from fplinux_cli.runtime.data_paths import archive_runtime_root

from tests.distribution_support import (
    RELEASE_ID,
    SELECTIONS,
    json_bytes,
    release_fields,
    write_zip,
)


def _write_distribution(
    directory: Path, files: dict[str, bytes], *, device_data: dict[str, str] | None
) -> Path:
    directory.mkdir()
    records = []
    for target, host in SELECTIONS:
        runtime = directory / f"FPLinux-{target}-{host}-x64-{RELEASE_ID}.zip"
        write_zip(
            runtime,
            {**files, "runtime-manifest.json": json_bytes({"device_data": device_data})},
            {"Run.sh"},
        )
        records.append(
            {
                "archive": runtime.name,
                "target": target,
                "host": host,
                "identity": "f" * 64,
                "sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
            }
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
        tmp_path / "distribution", {"README.txt": b"native smoke archive\n"}, device_data={}
    )


@pytest.mark.parametrize("storage_status", [0, 1], ids=["success", "storage-failure"])
def test_native_smoke_extracts_under_prepared_permissions_and_cleans_up(
    distribution: Path, monkeypatch: pytest.MonkeyPatch, storage_status: int
) -> None:
    original_files = set(distribution.iterdir())
    launched_roots: list[Path] = []

    def launch(root: Path, host: str, scope: Path, *, attempt: int) -> None:
        assert host == "windows"
        assert attempt in (1, 2)
        assert (root / "README.txt").read_bytes() == b"native smoke archive\n"
        assert scope == archive_runtime_root(root / ".data", root)
        for relative in (".", "logs", "session", "reader/session", "temp"):
            assert (scope / relative).is_dir()
        launched_roots.append(root)

    def run_storage(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        assert kwargs["cwd"] == launched_roots[-1]
        return subprocess.CompletedProcess(command, storage_status)

    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)
    monkeypatch.setattr(smoke, "_launch_to_invitation", launch)
    monkeypatch.setattr(subprocess, "run", run_storage)

    if storage_status:
        with pytest.raises(SystemExit, match="native storage creation/reopening failed"):
            smoke.smoke_distribution(distribution, host="windows")
    else:
        smoke.smoke_distribution(distribution, host="windows")

    windows_archives = sum(host == "windows" for _target, host in SELECTIONS)
    expected_launches = 1 if storage_status else 2 * windows_archives
    assert len(launched_roots) == expected_launches
    assert all(root.parent.parent == distribution for root in launched_roots)
    assert all(not root.exists() for root in launched_roots)
    assert set(distribution.iterdir()) == original_files


@pytest.mark.skipif(os.name != "posix", reason="the launcher peer uses POSIX shell")
@pytest.mark.parametrize(
    ("device_data", "storage_paths"),
    [
        (
            {},
            (".", "logs", "session", "temp", "reader/session"),
        ),
        (None, (".", "logs", "session", "temp")),
    ],
    ids=["preparation", "direct-boot"],
)
def test_native_smoke_runs_relative_distribution_launchers_twice_and_cleans_up(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    device_data: dict[str, str] | None,
    storage_paths: tuple[str, ...],
) -> None:
    launcher = Path(__file__).parent / "fixtures/native-smoke/launcher.sh"
    directory = _write_distribution(
        tmp_path / "distribution", {"Run.sh": launcher.read_bytes()}, device_data=device_data
    )
    original_files = set(directory.iterdir())
    storage_roots: list[Path] = []

    def run_storage(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        root = kwargs["cwd"]
        assert isinstance(root, Path)
        scope = archive_runtime_root(root / ".data", root)
        for relative in storage_paths:
            assert (scope / relative).is_dir()
        assert (scope / "reader").exists() == ("reader/session" in storage_paths)
        assert tuple(command[command.index(str(root)) + 1 :]) == tuple(
            str(scope / relative) for relative in storage_paths
        )
        storage_roots.append(root)
        attempt = storage_roots.count(root)
        assert (
            sorted(path.parent.name for path in (scope / "logs").glob("*/runtime.log"))
            == [
                "smoke-events-1",
                "smoke-events-2",
            ][:attempt]
        )
        for index in range(1, attempt + 1):
            assert (scope / f"smoke-events-{index}.jsonl").read_bytes() == (
                b'{"event":"waiting-for-device"}\n'
            )
            log_directory = scope / f"logs/smoke-events-{index}"
            assert (log_directory / "runtime.log").read_bytes() == b"launcher initialized\n"
            assert (log_directory / "stopped").read_bytes() == b"stopped\n"
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)
    monkeypatch.setattr(subprocess, "run", run_storage)

    smoke.smoke_distribution(Path("distribution"), host="linux")

    linux_archives = sum(host == "linux" for _target, host in SELECTIONS)
    assert len(storage_roots) == 2 * linux_archives
    assert all(not root.exists() for root in storage_roots)
    assert set(directory.iterdir()) == original_files


@pytest.mark.skipif(os.name != "posix", reason="the launcher peer uses POSIX shell")
def test_native_smoke_preserves_launcher_stderr_after_workspace_cleanup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    launcher = Path(__file__).parent / "fixtures/native-smoke/failing-launcher.sh"
    directory = _write_distribution(
        tmp_path / "distribution", {"Run.sh": launcher.read_bytes()}, device_data=None
    )
    original_files = set(directory.iterdir())
    monkeypatch.setattr(smoke, "_require_ordinary_account", lambda _host: None)

    with pytest.raises(
        SystemExit, match="native launcher exited before its invitation"
    ) as failure:
        smoke.smoke_distribution(directory, host="linux")

    assert "fixture launcher cannot initialize" in str(failure.value)
    assert set(directory.iterdir()) == original_files
