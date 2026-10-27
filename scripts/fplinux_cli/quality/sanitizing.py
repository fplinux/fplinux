# SPDX-License-Identifier: GPL-2.0-only
"""Verify the native sanitizer runtime and retain every diagnostic finding."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail

if TYPE_CHECKING:
    from typing import Literal

_FINDING = re.compile(
    r"ERROR:|runtime error:|AddressSanitizer:DEADLYSIGNAL|Sanitizer CHECK failed"
)


def collect_reports(directory: Path) -> list[Path]:
    """Return findings independently of pytest and child exit-status expectations."""
    return [
        report
        for report in sorted(directory.glob("report.*"))
        if _FINDING.search(report.read_text(errors="replace"))
    ]


def _verify_canary(
    source: str,
    marker: str,
    reports: Path,
    build: Path,
    *,
    scenario: Literal["plain", "closed-stderr", "wrapped-open"] = "plain",
) -> None:
    """Retain an empty-environment child's complete diagnostic before checking it."""
    name = source.removesuffix(".c") + (f"-{scenario}" if scenario != "plain" else "")
    directory = reports / name
    directory.mkdir(parents=True)
    executable = build / name
    environment = os.environ | {"FPLINUX_SANITIZER_LOG_ROOT": str(directory)}
    arguments = ["cc", "-O0", str(ROOT / "tests/sanitizer" / source)]
    if scenario == "wrapped-open":
        arguments.extend(
            [
                str(ROOT / "tests/host_tool/runtime/fplinux-kmsg-sink.c"),
                "-Wl,--wrap=open",
            ]
        )
    subprocess.run(
        [*arguments, "-o", str(executable)],
        env=environment,
        check=True,
        timeout=30,
    )
    try:
        result = subprocess.run(
            [str(executable), *(["close-stderr"] if scenario == "closed-stderr" else [])],
            env={},
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        timeout_output = (error.stdout or b"") + (error.stderr or b"")
        (directory / "process.log").write_bytes(timeout_output)
        fail(f"sanitizer canary {source} timed out after 30s; reports: {directory}")
    output = result.stdout + result.stderr
    (directory / "process.log").write_text(output)
    findings = collect_reports(directory)
    report = "".join(path.read_text(errors="replace") for path in findings)
    if (
        result.returncode == 0
        or marker not in report
        or re.search(re.escape(source) + r":\d+", report) is None
    ):
        fail(
            f"sanitizer canary {source} failed (exit {result.returncode}); "
            f"reports: {directory}\n{report}{output}"
        )
    print(
        f"sanitizer canary {name}: {marker}, exit {result.returncode}, source located",
        flush=True,
    )


def _verify_expected_child(reports: Path) -> None:
    """Verify that an expected child failure still produces an independent finding."""
    directory = reports / "expected-child"
    directory.mkdir()
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "--disable-plugin-autoload",
            "-o",
            f"cache_dir={directory / 'pytest-cache'}",
            str(ROOT / "tests/sanitizer/fixtures/expected_child.py"),
        ],
        env=os.environ | {"FPLINUX_SANITIZER_LOG_ROOT": str(directory)},
        capture_output=True,
        text=True,
        timeout=45,
        check=False,
    )
    (directory / "pytest.log").write_text(result.stdout + result.stderr)
    findings = collect_reports(directory)
    if result.returncode or not any(
        "runtime error: signed integer overflow" in path.read_text() for path in findings
    ):
        fail(f"expected-child sanitizer canary failed; reports: {directory}")
    print("sanitizer canary expected child failure: pytest exit 0, finding retained", flush=True)


def main() -> None:
    """Prove fatal UB reporting inside the actual quality runtime."""
    reports = Path(os.environ["FPLINUX_SANITIZER_LOG_ROOT"]) / "canaries"
    with tempfile.TemporaryDirectory(prefix="fplinux-sanitizer-") as temporary:
        build = Path(temporary)
        _verify_canary("undefined.c", "runtime error: signed integer overflow", reports, build)
        _verify_canary(
            "undefined.c",
            "runtime error: signed integer overflow",
            reports,
            build,
            scenario="closed-stderr",
        )
        _verify_canary(
            "undefined.c",
            "runtime error: signed integer overflow",
            reports,
            build,
            scenario="wrapped-open",
        )
        _verify_expected_child(reports)


if __name__ == "__main__":
    main()
