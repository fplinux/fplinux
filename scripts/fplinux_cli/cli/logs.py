# SPDX-License-Identifier: GPL-2.0-only
"""Read command journals without starting a runtime or blocking their writers."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import deque
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.output import silence_broken_pipe

if TYPE_CHECKING:
    from collections.abc import Iterator

_STATUSES = ("running", "success", "failed", "interrupted")


@dataclass(frozen=True)
class LogStage:
    """One journaled stage and the output file owned by its reporter."""

    name: str
    path: Path
    status: str


@dataclass(frozen=True)
class LogRun:
    """The existing reporter's timestamps and stages, without a second index."""

    path: Path
    started: datetime
    finished: datetime | None
    status: str
    pid: int
    parent: str | None
    stages: tuple[LogStage, ...]


def read_run(path: Path) -> LogRun | None:
    """Ignore absent or unrecognized journals; reporters publish complete JSON atomically."""
    try:
        data = json.loads((path / "run.json").read_text())
        if not isinstance(data, dict) or data["status"] not in _STATUSES:
            return None
        started = datetime.fromisoformat(data["started_at"])
        finished = datetime.fromisoformat(data["finished_at"]) if data["finished_at"] else None
        if started.tzinfo is None or (finished is not None and finished.tzinfo is None):
            return None
        stages = []
        for stage in data["stages"]:
            # A stage log is a sibling, never an arbitrary file selected by metadata.
            filename = stage["log"]
            if Path(filename).name != filename or not filename.endswith(".log"):
                return None
            stages.append(LogStage(stage["name"], path / filename, stage["status"]))
        return LogRun(
            path,
            started,
            finished,
            data["status"],
            int(data["pid"]),
            data["parent"],
            tuple(stages),
        )
    except OSError, ValueError, TypeError, KeyError:
        return None


def discover_runs(root: Path) -> Iterator[LogRun]:
    """Discover top-level invocations, not their container reporters or unrelated files."""
    for directory, children, files in root.walk():
        if "run.json" not in files:
            continue
        children.clear()
        run = read_run(directory)
        if run is not None and run.parent is None:
            yield run


def run_details(run: LogRun, root: Path) -> dict[str, object]:
    """Describe a run using its recorded time and command-owned directory context."""
    relative = run.path.relative_to(root)
    context = list(relative.parts[1:-1])
    profile = None
    if len(context) >= 2 and context[-2] == "profiles":
        profile = context.pop()
        context.pop()
    elif relative.parts[0] in {"check", "build"}:
        profile = "default"
    return {
        "id": relative.as_posix(),
        "command": relative.parts[0],
        "target": "/".join(context) or None,
        "profile": profile,
        "status": run.status,
        "started_at": run.started.isoformat(),
        "finished_at": run.finished.isoformat() if run.finished is not None else None,
        "duration_seconds": round(
            ((run.finished or datetime.now(UTC)) - run.started).total_seconds(), 3
        ),
        "pid": run.pid,
        "path": (Path(".cache/logs") / relative).as_posix(),
    }


def _tail_count(text: str) -> int:
    try:
        count = int(text)
    except ValueError as error:
        message = "must be a non-negative integer"
        raise argparse.ArgumentTypeError(message) from error
    if count < 0:
        message = "must be a non-negative integer"
        raise argparse.ArgumentTypeError(message)
    return count


def add_log_arguments(parser: argparse.ArgumentParser) -> None:
    """Expose read-only listing, bounded inspection and following of one invocation."""
    actions = parser.add_subparsers(dest="logs_action", required=True)
    for action, help_text in (
        ("list", "list recorded command runs, newest first"),
        ("show", "show stage output from one run"),
        ("follow", "follow stage output until the selected run finishes"),
    ):
        child = actions.add_parser(action, help=help_text)
        child.add_argument(
            "--command", dest="logs_command", metavar="NAME", help="filter by command name"
        )
        child.add_argument("--target", help="filter by target name")
        child.add_argument(
            "--profile", dest="logs_profile", metavar="NAME", help="filter by build/check profile"
        )
        child.add_argument("--status", choices=_STATUSES, help="filter by recorded run status")
        if action == "list":
            child.add_argument("--json", action="store_true", help="print run records as JSON")
        else:
            child.add_argument(
                "run", nargs="?", default="latest", help="run ID or latest (default)"
            )
            child.add_argument("--stage", help="stage name or path shown in log headings")
            child.add_argument(
                "--tail",
                type=_tail_count,
                default=40,
                metavar="N",
                help="initial lines per stage (default: 40; 0: none)",
            )
            if action == "show":
                child.add_argument(
                    "--failed", action="store_true", help="show only failed or interrupted stages"
                )


def _stage_logs(run: LogRun) -> list[LogStage]:
    """Include child reporters because compact parent logs omit their full tool output."""
    records = [run]
    for receipt in run.path.rglob("run.json"):
        if receipt.parent == run.path:
            continue
        child = read_run(receipt.parent)
        if child is not None and child.parent is not None:
            records.append(child)
    records.sort(key=lambda item: (item.started, str(item.path)))
    return [stage for record in records for stage in record.stages]


def _stage_id(stage: LogStage, run: LogRun) -> str:
    parent = stage.path.parent.relative_to(run.path)
    return (parent / stage.name).as_posix()


def _copy_log(path: Path, offset: int | None, tail: int) -> int:
    """Print the initial tail or only newly appended bytes, retaining no complete log in memory."""
    with path.open("rb") as stream:
        if offset is None:
            if tail:
                for line in deque(stream, maxlen=tail):
                    sys.stdout.buffer.write(line)
            else:
                stream.seek(0, os.SEEK_END)
        else:
            stream.seek(offset)
            # Limit this pass to the current file size so a busy writer cannot starve others.
            remaining = os.fstat(stream.fileno()).st_size - offset
            while remaining > 0:
                chunk = stream.read(min(remaining, 64 * 1024))
                if not chunk:
                    break
                sys.stdout.buffer.write(chunk)
                remaining -= len(chunk)
        sys.stdout.buffer.flush()
        return stream.tell()


def _show_run(run: LogRun, args: argparse.Namespace) -> None:
    offsets: dict[Path, int] = {}
    matched = False
    last_path = None
    initial = True
    while True:
        current = read_run(run.path)
        if current is None:
            fail(f"run journal is no longer available: {run.path}")
        for stage in _stage_logs(current):
            identity = _stage_id(stage, current)
            if args.stage is not None and args.stage not in {stage.name, identity}:
                continue
            if getattr(args, "failed", False) and stage.status not in {"failed", "interrupted"}:
                continue
            matched = True
            offset = offsets.get(stage.path)
            if offset is None and not initial:
                offset = 0
            size = stage.path.stat().st_size
            if offset is None or size > offset:
                if last_path != stage.path:
                    print(f"\n==> {identity} [{stage.status}] <==", flush=True)
                    last_path = stage.path
                offsets[stage.path] = _copy_log(stage.path, offset, args.tail)
        if args.logs_action != "follow" or current.status != "running":
            if args.stage is not None and not matched:
                fail(f"stage not found: {args.stage}")
            if args.logs_action == "follow":
                print(f"run finished: {current.status}", flush=True)
            return
        try:
            os.kill(current.pid, 0)
        except ProcessLookupError:
            # The writer can finish between our metadata read and the process check.
            final = read_run(run.path)
            if final is not None and final.status != "running":
                continue
            fail("run is recorded as running, but its process has exited without a final status")
        except PermissionError:
            pass
        initial = False
        time.sleep(0.25)


def read_logs(args: argparse.Namespace) -> None:
    """Read existing journals without cache locks, runtime setup or new receipts."""
    root = ROOT / ".cache/logs"
    runs = []
    for run in discover_runs(root):
        details = run_details(run, root)
        if all(
            value is None or value == details[key]
            for key, value in (
                ("command", args.logs_command),
                ("target", args.target),
                ("profile", args.logs_profile),
                ("status", args.status),
            )
        ):
            runs.append((run, details))
    runs.sort(key=lambda item: (item[0].started, str(item[0].path)), reverse=True)
    try:
        if args.logs_action == "list":
            if args.json:
                print(json.dumps([details for _run, details in runs], indent=2))
            else:
                print("RUN COMMAND TARGET PROFILE STATUS STARTED DURATION")
                for _run, details in runs:
                    print(
                        " ".join(
                            str(details[key]) if details[key] is not None else "-"
                            for key in (
                                "id",
                                "command",
                                "target",
                                "profile",
                                "status",
                                "started_at",
                                "duration_seconds",
                            )
                        )
                    )
            return
        selected = (
            runs[:1]
            if args.run == "latest"
            else [item for item in runs if args.run in {item[1]["id"], item[0].path.name}]
        )
        if not selected:
            fail(f"no matching log run: {args.run}")
        if len(selected) > 1:
            fail(f"ambiguous run ID: {args.run}; use the full ID from logs list")
        run, details = selected[0]
        print(f"{details['id']}: {run.status}", flush=True)
        _show_run(run, args)
    except BrokenPipeError:
        silence_broken_pipe(sys.stdout)
