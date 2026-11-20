# SPDX-License-Identifier: GPL-2.0-only
"""Read command journals without starting a runtime or blocking their writers."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import time
from collections import deque
from dataclasses import dataclass, replace
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.reporting.process import silence_broken_pipe

if TYPE_CHECKING:
    from collections.abc import Iterator

_STATUSES = ("running", "success", "failed", "interrupted")
_BUILD_TYPES = ("release", "debug")


@dataclass(frozen=True)
class LogStage:
    """One journaled stage and the output file owned by its reporter."""

    name: str
    path: Path
    status: str
    exit_code: int | None = None
    unfinished_parent_status: str | None = None


@dataclass(frozen=True)
class LogRun:
    """The existing reporter's timestamps and stages, without a second index."""

    path: Path
    started: datetime
    finished: datetime | None
    status: str
    pid: int
    parent: str | None
    display_root: str | None
    build_type: str | None
    stages: tuple[LogStage, ...]


def read_run(path: Path) -> LogRun | None:
    """Ignore absent or unrecognized journals; reporters publish complete JSON atomically."""
    try:
        receipt = path / "run.json"
        data = json.loads(receipt.read_text()) if receipt.is_file(follow_symlinks=False) else None
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
            exit_code = stage.get("exit")
            if exit_code is not None and type(exit_code) is not int:
                return None
            stages.append(LogStage(stage["name"], path / filename, stage["status"], exit_code))
        build_type = data.get("build_type")
        return LogRun(
            path,
            started,
            finished,
            data["status"],
            int(data["pid"]),
            data["parent"],
            data.get("display_root"),
            build_type if build_type in _BUILD_TYPES else None,
            tuple(stages),
        )
    except OSError, ValueError, TypeError, KeyError:
        return None


def discover_runs(root: Path) -> Iterator[LogRun]:
    """Discover top-level invocations, not their container reporters or unrelated files."""
    if root.is_symlink():
        return
    for directory, children, files in root.walk():
        if "run.json" not in files:
            continue
        children.clear()
        run = read_run(directory)
        if directory != root and run is not None and run.parent is None:
            yield run


def _run_candidates(root: Path, selector: str) -> Iterator[LogRun]:
    """Use a complete ID directly; discover journals for latest or a basename."""
    if "/" not in selector:
        yield from discover_runs(root)
        return
    relative = PurePosixPath(selector)
    if relative.is_absolute() or ".." in relative.parts or relative.as_posix() != selector:
        return
    if root.is_symlink():
        return
    directory = root
    for component in relative.parts:
        if (directory / "run.json").exists(follow_symlinks=False):
            return
        directory /= component
        if not directory.is_dir(follow_symlinks=False):
            return
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
        "build_type": run.build_type,
        "status": run.status,
        "started_at": run.started.isoformat(),
        "duration_seconds": round(
            ((run.finished or datetime.now(UTC)) - run.started).total_seconds(), 3
        ),
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


def _log_pattern(text: str) -> re.Pattern[str]:
    try:
        return re.compile(text)
    except re.PatternError as error:
        raise argparse.ArgumentTypeError(f"invalid regular expression: {error}") from error


def add_log_arguments(parser: argparse.ArgumentParser) -> None:
    """Expose read-only listing, inspection, and following of recorded invocations."""
    actions = parser.add_subparsers(dest="logs_action", required=True)
    for action, help_text in (
        ("list", "list recorded command runs, newest first"),
        ("show", "show stage output from one run"),
        ("follow", "follow stage output until the selected run finishes"),
        ("artifacts", "list retained artifacts from one run"),
    ):
        child = actions.add_parser(action, help=help_text)
        child.add_argument(
            "--command", dest="logs_command", metavar="NAME", help="filter by command name"
        )
        child.add_argument("--target", help="filter by target name")
        child.add_argument(
            "--profile", dest="logs_profile", metavar="NAME", help="filter by build/check profile"
        )
        child.add_argument(
            "--build-type", choices=_BUILD_TYPES, help="filter by recorded build/check type"
        )
        child.add_argument("--status", choices=_STATUSES, help="filter by recorded run status")
        if action == "list":
            child.add_argument(
                "--limit", type=_tail_count, metavar="N", help="show at most N newest runs"
            )
        else:
            child.add_argument(
                "run", nargs="?", default="latest", help="run ID or latest (default)"
            )
        if action in {"show", "follow"}:
            child.add_argument("--stage", help="stage name or path shown in log headings")
            tail_help = (
                "last N lines or matches per stage (default: 40; 0: none)"
                if action == "show"
                else "last N lines per stage initially (default: 40; 0: none)"
            )
            child.add_argument("--tail", type=_tail_count, default=40, metavar="N", help=tail_help)
        if action == "show":
            output = child.add_mutually_exclusive_group()
            output.add_argument(
                "--summary",
                action="store_true",
                help="show stage status and exit without logs",
            )
            output.add_argument(
                "--grep",
                type=_log_pattern,
                metavar="PATTERN",
                help="search the complete selected stage logs for a regular expression",
            )
            child.add_argument(
                "--json",
                action="store_true",
                help="print a JSON stage summary (requires --summary)",
            )
            child.add_argument(
                "--max-bytes",
                type=_tail_count,
                metavar="N",
                help="limit log content bytes across all selected stages",
            )
            child.add_argument(
                "--failed",
                action="store_true",
                help="show failed, interrupted or unfinished stages of an unsuccessful run",
            )
        if action == "artifacts":
            child.add_argument("--json", action="store_true", help="print a JSON artifact list")


def _owned_log_tree(run: LogRun) -> Iterator[tuple[Path, list[str], LogRun | None]]:
    """Walk one run and admit nested reporters only with matching recorded ownership."""
    for directory, children, files in run.path.walk():
        record = None
        if directory == run.path:
            record = run
        elif "run.json" in files:
            if run.display_root is not None:
                child = read_run(directory)
                expected = PurePosixPath(run.display_root) / directory.relative_to(run.path)
                if (
                    child is not None
                    and child.display_root == expected.as_posix()
                    and child.parent == expected.parent.as_posix()
                ):
                    record = child
            if record is None:
                children.clear()
                continue
        yield directory, files, record


def _stage_logs(run: LogRun) -> list[LogStage]:
    """Include child reporters because compact parent logs omit their full tool output."""
    if run.display_root is None:
        return list(run.stages)
    records = [record for _, _, record in _owned_log_tree(run) if record is not None]
    records.sort(key=lambda item: (item.started, str(item.path)))
    stages = []
    for record in records:
        for stage in record.stages:
            display_stage = stage
            if (
                record is not run
                and stage.status == "running"
                and run.status
                in {
                    "failed",
                    "interrupted",
                }
            ):
                display_stage = replace(stage, unfinished_parent_status=run.status)
            stages.append(display_stage)
    return stages


def _stage_id(stage: LogStage, run: LogRun) -> str:
    parent = stage.path.parent.relative_to(run.path)
    return (parent / stage.name).as_posix()


def _matching_stages(
    run: LogRun, stages: list[LogStage], args: argparse.Namespace
) -> Iterator[tuple[str, LogStage]]:
    for stage in stages:
        identity = _stage_id(stage, run)
        if args.stage is not None and args.stage not in {stage.name, identity}:
            continue
        if (
            getattr(args, "failed", False)
            and stage.status not in {"failed", "interrupted"}
            and stage.unfinished_parent_status is None
        ):
            continue
        yield identity, stage


def _require_stage_match(run: LogRun, stages: list[LogStage], requested: str | None) -> None:
    if requested is not None:
        available = "\n".join(f"  {_stage_id(stage, run)}" for stage in stages)
        fail(f"stage not found: {requested}\navailable stage IDs:\n" + (available or "  (none)"))


def _stage_size(path: Path) -> int:
    metadata = path.stat(follow_symlinks=False)
    if not stat.S_ISREG(metadata.st_mode):
        fail(f"stage log is not a regular file: {path}")
    return metadata.st_size


def _grep_stage(
    path: Path, pattern: re.Pattern[str], tail: int
) -> tuple[int, deque[tuple[int, str]]]:
    """Search a snapshot of the stage and retain only the requested last matches."""
    matches: deque[tuple[int, str]] = deque(maxlen=tail)
    count = 0
    remaining = _stage_size(path)
    with path.open("rb") as stream:
        line_number = 0
        while remaining > 0:
            line = stream.readline(remaining)
            if not line:
                break
            remaining -= len(line)
            line_number += 1
            text = line.decode("utf-8", errors="replace").removesuffix("\n")
            if pattern.search(text):
                count += 1
                matches.append((line_number, text))
    return count, matches


def _grep_run(run: LogRun, args: argparse.Namespace) -> None:
    stages = _stage_logs(run)
    selected = list(_matching_stages(run, stages, args))
    if not selected:
        _require_stage_match(run, stages, args.stage)
        if args.failed:
            print("no failed or interrupted stages in this run")
            return
    budget = args.max_bytes
    matched = 0
    omitted_by_tail = 0
    clipped = False
    for identity, stage in selected:
        count, lines = _grep_stage(stage.path, args.grep, args.tail)
        matched += count
        omitted_by_tail += count - len(lines)
        for line_number, text in lines:
            if budget is not None:
                size = len(text.encode("utf-8")) + 1
                if size > budget:
                    clipped = True
                    break
                budget -= size
            print(f"{identity}:{line_number}:{text}")
        if clipped:
            budget = 0
    if not matched:
        print("no matching log lines")
    if omitted_by_tail:
        print(f"log matches truncated by --tail ({omitted_by_tail} omitted)")
    if clipped:
        print("log matches truncated by --max-bytes")


def _json_summary(run: LogRun, details: dict[str, object], args: argparse.Namespace) -> None:
    stages = _stage_logs(run)
    selected = [
        {
            "id": identity,
            "status": stage.status,
            "exit_code": stage.exit_code,
            "unfinished_parent_status": stage.unfinished_parent_status,
        }
        for identity, stage in _matching_stages(run, stages, args)
    ]
    if not selected:
        _require_stage_match(run, stages, args.stage)
    print(json.dumps({"run": details, "stages": selected}, indent=2, sort_keys=True))


def _show_artifacts(run: LogRun, details: dict[str, object], *, json_output: bool) -> None:
    artifacts: list[str] = []
    for directory, files, record in _owned_log_tree(run):
        excluded = {"run.json"}
        if record is not None:
            excluded.update(stage.path.name for stage in record.stages)
        for filename in files:
            if filename in excluded or filename.startswith(".run.json."):
                continue
            path = directory / filename
            if path.is_file(follow_symlinks=False):
                artifacts.append(str(path))
    artifacts.sort()
    if json_output:
        print(json.dumps({"run": details, "artifacts": artifacts}, indent=2, sort_keys=True))
    elif artifacts:
        for path_text in artifacts:
            print(path_text)
    else:
        print("no artifacts in this run")


def _copy_log(
    path: Path, offset: int | None, tail: int, *, budget: int | None
) -> tuple[int, int, bool]:
    """Print the initial tail or only newly appended bytes, retaining no complete log in memory."""
    with path.open("rb") as stream:
        written = 0
        truncated = False
        if offset is None:
            if not tail:
                stream.seek(0, os.SEEK_END)
            elif budget is None:
                for line in deque(stream, maxlen=tail):
                    written += sys.stdout.buffer.write(line)
            else:
                stream.seek(0, os.SEEK_END)
                size = stream.tell()
                available = min(size, budget)
                stream.seek(size - available)
                data = stream.read(available)
                lines = data.split(b"\n") if data else []
                if data.endswith(b"\n"):
                    lines.pop()
                selected = b"\n".join(lines[-tail:])
                if data.endswith(b"\n") and lines:
                    selected += b"\n"
                truncated = available < size and len(lines) <= tail
                if truncated and len(lines) == tail:
                    stream.seek(size - available - 1)
                    truncated = stream.read(1) != b"\n"
                    stream.seek(size)
                if truncated:
                    selected = selected.lstrip(bytes(range(0x80, 0xC0)))
                written = sys.stdout.buffer.write(selected)
        else:
            stream.seek(offset)
            # Limit this pass to the current file size so a busy writer cannot starve others.
            remaining = os.fstat(stream.fileno()).st_size - offset
            while remaining > 0:
                chunk = stream.read(min(remaining, 64 * 1024))
                if not chunk:
                    break
                written += sys.stdout.buffer.write(chunk)
                remaining -= len(chunk)
        sys.stdout.buffer.flush()
        return stream.tell(), written, truncated


def _show_run(run: LogRun, args: argparse.Namespace) -> None:
    offsets: dict[Path, int] = {}
    matched = False
    last_path = None
    initial = True
    budget = args.max_bytes if args.logs_action == "show" else None
    truncated = False
    while True:
        current = read_run(run.path)
        if current is None:
            fail(f"run journal is no longer available: {run.path}")
        stages = _stage_logs(current)
        if args.logs_action == "show" and args.summary:
            print("STAGE STATUS EXIT")
        for identity, stage in _matching_stages(current, stages, args):
            matched = True
            if args.logs_action == "show" and args.summary:
                status = stage.status
                if stage.unfinished_parent_status is not None:
                    status += f"; unfinished; parent {stage.unfinished_parent_status}"
                exit_code = "-" if stage.exit_code is None else str(stage.exit_code)
                print(f"{identity} {status} {exit_code}")
                continue
            offset = offsets.get(stage.path)
            if offset is None and not initial:
                offset = 0
            size = _stage_size(stage.path)
            if offset is None or size > offset:
                if last_path != stage.path:
                    status = stage.status
                    if stage.unfinished_parent_status is not None:
                        status += f"; unfinished; parent {stage.unfinished_parent_status}"
                    print(f"\n==> {identity} [{status}] <==", flush=True)
                    last_path = stage.path
                position, written, clipped = _copy_log(
                    stage.path, offset, args.tail, budget=budget
                )
                offsets[stage.path] = position
                if budget is not None:
                    budget -= written
                truncated = truncated or clipped
        if args.logs_action != "follow" or current.status != "running":
            if not matched:
                _require_stage_match(current, stages, args.stage)
            if getattr(args, "failed", False) and not matched:
                print("no failed or interrupted stages in this run", flush=True)
            if truncated:
                print("log content truncated by --max-bytes; omit it to see the selected tails")
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
    root = data_root(ROOT) / "logs"
    runs = []
    for run in _run_candidates(root, getattr(args, "run", "latest")):
        details = run_details(run, root)
        if all(
            value is None or value == details[key]
            for key, value in (
                ("command", args.logs_command),
                ("target", args.target),
                ("profile", args.logs_profile),
                ("build_type", args.build_type),
                ("status", args.status),
            )
        ):
            runs.append((run, details))
    runs.sort(key=lambda item: (item[0].started, str(item[0].path)), reverse=True)
    try:
        if args.logs_action == "list":
            print("RUN COMMAND TARGET PROFILE BUILD_TYPE STATUS STARTED DURATION")
            selected_runs = runs if args.limit is None else runs[: args.limit]
            for _run, details in selected_runs:
                row = []
                for key in (
                    "id",
                    "command",
                    "target",
                    "profile",
                    "build_type",
                    "status",
                    "started_at",
                    "duration_seconds",
                ):
                    value = details[key]
                    if value is None:
                        value = "unknown" if key == "build_type" else "-"
                    row.append(str(value))
                print(" ".join(row))
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
        if getattr(args, "json", False):
            if args.logs_action == "artifacts":
                _show_artifacts(run, details, json_output=True)
            else:
                _json_summary(run, details, args)
            return
        context = ""
        if details["command"] in {"build", "check"}:
            context = f"; build type: {run.build_type or 'unknown'}"
        print(f"{details['id']}: {run.status}{context}", flush=True)
        if args.logs_action == "artifacts":
            _show_artifacts(run, details, json_output=False)
        elif args.logs_action == "show" and args.grep is not None:
            _grep_run(run, args)
        else:
            _show_run(run, args)
    except BrokenPipeError:
        silence_broken_pipe(sys.stdout)
