# SPDX-License-Identifier: GPL-2.0-only
"""Dispatch fixed-bundle commands without entering a phone workflow."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.runtime import cli, workflow
from fplinux_cli.runtime import live as live_runtime

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize("allow_other_build", [False, True], ids=["strict", "diagnostic"])
def test_console_dispatches_to_saved_connection_without_starting_phone(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, allow_other_build: bool
) -> None:
    """The explicit console command preserves its selected remote action and exit status."""
    bundle = tmp_path / "bundle"
    data = tmp_path / "state"
    logs = tmp_path / "logs"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "fplinux.py",
            "console",
            "--data-root",
            str(data),
            "--log-root",
            str(logs),
            *(["--allow-other-build"] if allow_other_build else []),
            "--exec",
            "printf ready",
        ],
    )
    with (
        mock.patch.object(live_runtime, "console_bundle", return_value=7) as console,
        mock.patch.object(workflow, "run_phone", side_effect=AssertionError("fresh launch")),
        pytest.raises(SystemExit) as stopped,
    ):
        cli.main(bundle_root=bundle)

    assert stopped.value.code == 7
    console.assert_called_once_with(
        bundle,
        data,
        logs,
        allow_other_build=allow_other_build,
        exec_command="printf ready",
        upload=None,
        pull=None,
        screenshot=None,
        phone_data=None,
        verify_only=False,
        shutdown=False,
        connection=None,
    )


def test_console_rejects_phone_data_with_diagnostic_policy_before_dispatch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """An explicit phone-data selection cannot silently lose its build check."""
    monkeypatch.setattr(
        sys,
        "argv",
        ["fplinux.py", "console", "--allow-other-build", "--phone-data", "phone"],
    )
    with (
        mock.patch.object(live_runtime, "console_bundle") as console,
        pytest.raises(SystemExit) as stopped,
    ):
        cli.main(bundle_root=tmp_path)

    assert stopped.value.code == 2
    assert "--allow-other-build cannot be combined" in capsys.readouterr().err
    console.assert_not_called()


def test_verify_dispatches_only_to_strict_saved_connection(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Verification uses the archive identity and reports the check's status."""
    data = tmp_path / "state"
    logs = tmp_path / "logs"
    monkeypatch.setattr(
        sys,
        "argv",
        ["fplinux.py", "verify", "--data-root", str(data), "--log-root", str(logs)],
    )
    with (
        mock.patch.object(live_runtime, "console_bundle", return_value=1) as console,
        mock.patch.object(workflow, "run_phone", side_effect=AssertionError("fresh launch")),
        pytest.raises(SystemExit) as stopped,
    ):
        cli.main(bundle_root=tmp_path)

    assert stopped.value.code == 1
    console.assert_called_once_with(
        tmp_path,
        data,
        logs,
        allow_other_build=False,
        exec_command=None,
        upload=None,
        pull=None,
        screenshot=None,
        phone_data=None,
        verify_only=True,
        shutdown=False,
        connection=None,
    )


def test_fixed_bundle_console_dispatches_shutdown_as_a_live_action(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    state = tmp_path / "state"
    logs = tmp_path / "logs"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "fplinux.py",
            "console",
            "--shutdown",
            "--data-root",
            str(state),
            "--log-root",
            str(logs),
        ],
    )
    with (
        mock.patch.object(live_runtime, "console_bundle", return_value=1) as console,
        mock.patch.object(workflow, "run_phone", side_effect=AssertionError("fresh launch")),
        pytest.raises(SystemExit) as stopped,
    ):
        cli.main(bundle_root=tmp_path)

    assert stopped.value.code == 1
    assert console.call_args.args == (tmp_path, state, logs)
    assert console.call_args.kwargs["shutdown"] is True
    assert console.call_args.kwargs["verify_only"] is False
