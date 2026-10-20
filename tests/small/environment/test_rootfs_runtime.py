# SPDX-License-Identifier: GPL-2.0-only
"""Host component contracts for prepared-rootfs publication and exact reuse."""

from __future__ import annotations

import shutil
import subprocess
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.environment import runtime
from fplinux_cli.environment.state import (
    EnvironmentState,
    load_environment_state,
    publish_environment_state,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def prepared_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> tuple[Path, EnvironmentState, Path]:
    """Provide a previous published tree and a separately prepared replacement."""
    monkeypatch.setattr(runtime, "ROOT", tmp_path)
    previous = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    rootfs = tmp_path / ".cache/environment" / f"rootfs-{'b' * 64}"
    rootfs.mkdir(parents=True)
    (rootfs / "old-tool").write_bytes(b"working\n")
    publish_environment_state(tmp_path / ".cache", previous)
    candidate = rootfs.parent / "candidate-next"
    candidate.mkdir()
    (candidate / "new-tool").write_bytes(b"replacement\n")
    return rootfs, previous, candidate


def test_unverified_candidate_preserves_published_environment(
    prepared_environment: tuple[Path, EnvironmentState, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """Preparation failure leaves the previous consumer receipt and working tree intact."""
    rootfs, previous, candidate = prepared_environment
    monkeypatch.setattr(runtime, "probe_environment_state", lambda *_args: None)
    with pytest.raises(SystemExit, match="installed-content check"):
        runtime.publish_environment(
            "kern", candidate, EnvironmentState("d" * 64, "e" * 64, "f" * 64)
        )
    assert (rootfs / "old-tool").read_bytes() == b"working\n"
    assert load_environment_state(rootfs.parent.parent, previous.recipe) == previous
    assert (candidate / "new-tool").read_bytes() == b"replacement\n"


def test_receipt_failure_retains_old_tree_and_returns_candidate(
    prepared_environment: tuple[Path, EnvironmentState, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """A failed final receipt publication keeps the last-good environment addressable."""
    rootfs, previous, candidate = prepared_environment
    replacement = EnvironmentState("d" * 64, "e" * 64, "f" * 64)
    monkeypatch.setattr(runtime, "probe_environment_state", lambda *_args: replacement)
    with (
        mock.patch.object(runtime, "publish_environment_state", side_effect=OSError("no space")),
        pytest.raises(OSError, match="no space"),
    ):
        runtime.publish_environment("kern", candidate, replacement)
    assert load_environment_state(rootfs.parent.parent, previous.recipe) == previous
    assert (rootfs / "old-tool").read_bytes() == b"working\n"
    assert (candidate / "new-tool").read_bytes() == b"replacement\n"
    assert not (rootfs.parent / f"rootfs-{'e' * 64}").exists()


def test_successful_publication_replaces_only_previous_known_rootfs(
    prepared_environment: tuple[Path, EnvironmentState, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    """The complete replacement becomes current before the superseded tree is removed."""
    rootfs, _previous, candidate = prepared_environment
    replacement = EnvironmentState("d" * 64, "e" * 64, "f" * 64)
    unrelated = rootfs.parent / "user-archive"
    unrelated.mkdir()
    (unrelated / "saved").write_bytes(b"keep\n")
    monkeypatch.setattr(runtime, "probe_environment_state", lambda *_args: replacement)

    def remove_mapped_tree(path: Path) -> None:
        assert load_environment_state(rootfs.parent.parent, replacement.recipe) == replacement
        assert path == rootfs
        shutil.rmtree(path)

    monkeypatch.setattr(runtime, "remove_environment_rootfs", remove_mapped_tree)
    assert runtime.publish_environment("kern", candidate, replacement) == replacement
    published = runtime.environment_rootfs(rootfs.parent.parent, replacement)
    assert (published / "new-tool").read_bytes() == b"replacement\n"
    assert not rootfs.exists()
    assert not candidate.exists()
    assert (unrelated / "saved").read_bytes() == b"keep\n"


@pytest.mark.parametrize(
    "measured", ["c" * 64, "d" * 64], ids=["same-content", "different-content"]
)
def test_probe_requires_actual_content_match_in_ranged_namespace(
    tmp_path: Path, measured: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Rootfs reuse measures content and preserves logical UID mapping and tool PATH."""
    expected = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    monkeypatch.setattr(runtime, "kern_environment", dict)
    result = subprocess.CompletedProcess(
        [], 0, f"{'a' * 64}\n{'b' * 64}\n{'c' * 64}\n{measured}\n", ""
    )
    with mock.patch.object(subprocess, "run", return_value=result) as invoke:
        state = runtime.probe_environment_state("kern", tmp_path, "a" * 64)
    assert state == (expected if measured == "c" * 64 else None)
    command = invoke.call_args.args[0]
    assert command[command.index("--rootfs") + 1] == str(tmp_path)
    assert "--uid-range" in command
    assert "--read-only" in command
    assert "--bind-rootfs" not in command
    assert any(argument.startswith("PATH=/opt/quality/bin:") for argument in command)


def test_environment_probe_timeout_names_the_failed_boundary(tmp_path: Path) -> None:
    """A stuck runtime cannot leave callers waiting without a bounded diagnostic."""
    with (
        mock.patch.object(runtime, "kern_environment", return_value={}),
        mock.patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired(["kern"], 60)),
        pytest.raises(SystemExit, match="Kern environment lookup timed out"),
    ):
        runtime.probe_environment_state("kern", tmp_path, "a" * 64)
