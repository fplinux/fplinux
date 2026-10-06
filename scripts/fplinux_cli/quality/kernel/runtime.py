# SPDX-License-Identifier: GPL-2.0-only
"""Dispatch preparation and analysis while retaining each context's lifetime."""

from __future__ import annotations

import argparse
from functools import partial

from fplinux_cli.build.process import report_stage
from fplinux_cli.common import fail
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.reporting.run import RunReporter

from .analysis import check_one_context, context_object_count, record_text
from .contexts import context_label, load_sources, target_context, target_profiles
from .workers import _context_worker_command, _run_context_processes


def prepare_contexts(
    reporter: RunReporter | None, profile: str | None = None, *, build_type: str = "release"
) -> None:
    """Populate default contexts or one explicitly selected profile."""
    sources = load_sources()
    for target, selected in target_profiles(profile):
        label = context_label(target, selected)
        with report_stage(reporter, f"prepare-{label}"):
            _config, _platform, _source, prepared_linux = target_context(
                sources, target, selected, build_type=build_type
            )
            record_text(f"sparse context: ready ({label}, {prepared_linux.linux_recipe[:16]})\n")


def check_contexts(
    reporter: RunReporter | None,
    profile: str | None = None,
    *,
    jobs: int = 1,
    build_type: str = "release",
) -> None:
    """Run sparse through Kbuild for default or explicitly selected contexts."""
    if jobs < 1:
        message = "kernel check failed: jobs must be positive"
        fail(message)
    contexts = target_profiles(profile)
    if reporter is not None and reporter.verbose and jobs > 1:
        message = "kernel check failed: --verbose cannot use more than one job"
        fail(message)
    if len(contexts) == 1 or (jobs == 1 and (reporter is None or reporter.verbose)):
        sources = load_sources()
        checked = sum(
            check_one_context(reporter, sources, target, selected, build_type=build_type)
            for target, selected in contexts
        )
        print(f"sparse: OK ({checked} kernel C objects total)")
        return

    _run_context_processes(
        contexts, jobs, command_for=partial(_context_worker_command, build_type=build_type)
    )
    checked = sum(
        context_object_count(target, selected, build_type=build_type)
        for target, selected in contexts
    )
    print(f"sparse: OK ({checked} kernel C objects total)")


def main() -> None:
    """Dispatch the internal preparation or offline analysis phase."""
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "check"))
    parser.add_argument("--profile")
    parser.add_argument("--build-type", choices=BUILD_TYPES, default="release")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--context-target", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.context_target is not None:
        if args.phase != "check" or args.jobs != 1:
            parser.error("--context-target requires check --jobs 1")
        context = (args.context_target, args.profile)
        if context not in target_profiles(args.profile):
            parser.error(f"invalid kernel context: {context_label(*context)}")
        label = context_label(*context)
        reporter = RunReporter.from_environment("check", f"kernel-context-{label}")
        check_one_context(reporter, load_sources(), *context, build_type=args.build_type)
        return

    reporter = RunReporter.from_environment("check", f"kernel-{args.phase}")
    if args.phase == "prepare":
        if args.jobs != 1:
            parser.error("--jobs is supported only by the check phase")
        prepare_contexts(reporter, args.profile, build_type=args.build_type)
    else:
        check_contexts(reporter, args.profile, jobs=args.jobs, build_type=args.build_type)
