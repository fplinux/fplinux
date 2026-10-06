# SPDX-License-Identifier: GPL-2.0-only
"""Deterministic reporters replacing external checker processes."""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Self

if TYPE_CHECKING:
    from pathlib import Path


class _RecordingStage:
    """Collect fake container argv without starting a subprocess."""

    def __init__(self, commands: list[list[str]], reporter: _RecordingReporter, name: str) -> None:
        self.commands = commands
        self.reporter = reporter
        self.name = name

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_args: object) -> Literal[False]:
        return False

    def run(self, command: list[str], **_kwargs: object) -> None:
        self.commands.append(command)


class _FailingStage(_RecordingStage):
    """Fail the first attempted checker command."""

    def run(self, command: list[str], **_kwargs: object) -> None:
        self.commands.append(command)
        message = "scope failed"
        raise RuntimeError(message)


class _RecordingReporter:
    """Supply deterministic log plumbing to fake check stages."""

    def __init__(self, root: Path, commands: list[list[str]]) -> None:
        self.root = root
        self.commands = commands
        self.label = "check"

    def stage(self, name: str, **_kwargs: object) -> _RecordingStage:
        return _RecordingStage(self.commands, self, name)

    def container_environment(self, mounted_root: str) -> dict[str, str]:
        return {
            "FPLINUX_LOG_ROOT": mounted_root,
            "FPLINUX_LOG_DISPLAY_ROOT": ".cache/logs/test",
        }

    def finish(self) -> None:
        return None


class _FailingReporter(_RecordingReporter):
    def stage(self, name: str, **_kwargs: object) -> _RecordingStage:
        return _FailingStage(self.commands, self, name)
