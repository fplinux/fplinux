# SPDX-License-Identifier: GPL-2.0-only
"""Host preferences file format, atomic replacement and sync failures."""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture
def preferences_executable(tmp_path: Path) -> Path:
    executable = tmp_path / "preferences"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name("fplinux-ui-prefs.c")),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-Wl,--wrap=fsync",
            "-Wl,--wrap=mkdir",
            "-Wl,--wrap=fchmod",
            "-Wl,--wrap=mkostemp",
            "-Wl,--wrap=fflush",
            "-Wl,--wrap=fclose",
            "-Wl,--wrap=rename",
            "-Wl,--wrap=fopen",
            "-o",
            str(executable),
        ],
        name="compile UI preferences harness",
        timeout=30,
        check=True,
    )
    return executable


def test_preferences_round_trip_validation_and_durable_save(
    tmp_path: Path, preferences_executable: Path
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state)],
        name="run UI preferences harness",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"
    reported = [
        int(line.rsplit(":", 2)[1])
        for line in result.stderr.splitlines()
        if line.endswith("malformed preference ignored")
    ]
    assert reported == [2, 3, 4, 5, 6, 7, 11]


@pytest.mark.parametrize("operation", ["mkdir", "fchmod"])
def test_preferences_read_existing_store_without_metadata_writes(
    tmp_path: Path, preferences_executable: Path, operation: str
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), f"read-only-{operation}"],
        name="read existing UI preferences without metadata writes",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


def test_preferences_missing_directory_keeps_values_in_memory_until_save(
    tmp_path: Path, preferences_executable: Path
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), "missing-directory"],
        name="open UI preferences before its directory exists",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


@pytest.mark.parametrize("failure", ["access", "io"])
def test_preferences_read_error_keeps_existing_store(
    tmp_path: Path, preferences_executable: Path, failure: str
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), f"read-error-{failure}"],
        name="reject unreadable UI preferences without replacing them",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


@pytest.mark.parametrize("operation", ["temporary", "flush", "file-sync", "close", "rename"])
def test_preferences_failure_before_publication_retains_file_and_retry(
    tmp_path: Path, preferences_executable: Path, operation: str
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), f"save-error-{operation}"],
        name="retain UI preferences before a failed publication",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


def test_preferences_directory_sync_retry_keeps_published_file(
    tmp_path: Path, preferences_executable: Path
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), "directory-sync-retry"],
        name="finish UI preferences synchronization without republishing",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


def test_preferences_new_directory_sync_retry_retains_parent_publication(
    tmp_path: Path, preferences_executable: Path
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), "new-directory-sync-retry"],
        name="finish UI preferences parent directory synchronization",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"


def test_preferences_pending_sync_saves_latest_values(
    tmp_path: Path, preferences_executable: Path
) -> None:
    state = tmp_path / "state"
    state.mkdir()
    result = run_process(
        [str(preferences_executable), str(state), "pending-sync-latest"],
        name="save latest UI preferences while synchronization is pending",
        timeout=10,
        check=True,
    )
    assert result.stdout == "fplinux-ui-prefs: OK\n"
