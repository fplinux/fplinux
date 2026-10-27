# SPDX-License-Identifier: GPL-2.0-only
"""Public probe parser and path validation without a compiler or phone."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests.cli_support import ROOT, prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path


@dataclass(frozen=True)
class ProbeCli:
    """Own the isolated checkout used by the public probe command."""

    root: Path

    def run_probe(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Invoke the public command with bounded lifetime."""
        return run_process(
            [str(self.root / "fplinux"), "probe-build", *arguments],
            name="public probe-build validation",
            cwd=self.root,
            timeout=10,
        )


@pytest.fixture
def probe_cli(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ProbeCli:
    """Provide one source and no ambient build environment or cache."""
    prepare_cli_checkout(tmp_path)
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(tmp_path / ".data"))
    (tmp_path / "probe.c").write_text("int main(void) { return 0; }\n")
    return ProbeCli(tmp_path)


class ProbeCliTests:
    """Exercise the real entrypoint in an isolated checkout with no Kern state."""

    @pytest.mark.parametrize(
        "arguments",
        [
            pytest.param((), id="missing-source"),
            pytest.param(("probe.c",), id="missing-output"),
            pytest.param(
                ("probe.c", "probe.c", "--output", ".data/tools/probe"), id="extra-source"
            ),
            pytest.param(
                ("probe.c", "--output", ".data/tools/probe", "-static"), id="compiler-flag"
            ),
        ],
    )
    def test_parser_requires_one_source_and_explicit_output(
        self, probe_cli: ProbeCli, arguments: tuple[str, ...]
    ) -> None:
        """The command cannot infer output or accept arbitrary compiler flags."""
        help_result = probe_cli.run_probe("--help")
        assert (help_result.returncode) == (0), help_result.stderr
        assert ("SOURCE.c") in (help_result.stdout)
        assert ("--output PATH") in (help_result.stdout)
        result = probe_cli.run_probe(*arguments)
        assert (result.returncode) == (2), result.stderr

    @pytest.mark.parametrize(
        ("source", "output", "message"),
        [
            pytest.param(
                "../probe.c", ".data/tools/probe", "normalized relative path", id="outside-source"
            ),
            pytest.param(
                "./probe.c",
                ".data/tools/probe",
                "normalized relative path",
                id="unnormalized-source",
            ),
            pytest.param(
                None, ".data/tools/probe", "normalized relative path", id="absolute-source"
            ),
            pytest.param("missing.c", ".data/tools/probe", "regular .c file", id="missing-source"),
            pytest.param("fplinux", ".data/tools/probe", "regular .c file", id="non-c-source"),
            pytest.param("probe.c", "probe", "managed tools directory", id="outside-output"),
            pytest.param("probe.c", ".data/tools", "managed tools directory", id="tools-root"),
            pytest.param(
                "probe.c",
                ".data/tools/../probe",
                "normalized relative path",
                id="unnormalized-output",
            ),
        ],
    )
    def test_invalid_paths_are_rejected_before_environment_preparation(
        self, probe_cli: ProbeCli, source: str | None, output: str, message: str
    ) -> None:
        """Reject outside, unnormalized, missing and non-C inputs without building."""
        if source is None:
            source = str(probe_cli.root / "probe.c")
        result = probe_cli.run_probe(source, "--output", output)
        assert (result.returncode) == (1), result.stderr
        assert (message) in (result.stderr)
        assert not ((probe_cli.root / output).is_file())

    @pytest.mark.parametrize(
        ("source", "output", "message"),
        [
            pytest.param(
                "link.c", ".data/tools/probe", "must not use a symlink", id="source-symlink"
            ),
            pytest.param(
                "probe.c",
                ".data/tools/link/probe",
                "must not use a symlink",
                id="output-parent-symlink",
            ),
            pytest.param(
                "probe.c", ".data/tools/directory", "regular file", id="output-directory"
            ),
        ],
    )
    def test_symlinks_and_nonregular_outputs_are_refused(
        self, probe_cli: ProbeCli, source: str, output: str, message: str
    ) -> None:
        """Neither a source link nor a linked output parent can redirect the command."""
        tools = probe_cli.root / ".data/tools"
        tools.mkdir(parents=True)
        (probe_cli.root / "link.c").symlink_to("probe.c")
        (tools / "link").symlink_to(probe_cli.root, target_is_directory=True)
        (tools / "directory").mkdir()
        result = probe_cli.run_probe(source, "--output", output)
        assert (result.returncode) == (1), result.stderr
        assert (message) in (result.stderr)

    def test_explicit_private_source_reaches_the_setup_requirement(
        self, probe_cli: ProbeCli
    ) -> None:
        """An ignored local probe is accepted, but missing Kern never triggers setup."""
        for relative in (
            "environment.lock.toml",
            "sources.lock.toml",
            "package.json",
            "package-lock.json",
            "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
            "alpine/aports/fplinux-ui/APKBUILD",
            "alpine/aports/fplinux-font-terminus/APKBUILD",
            "alpine/aports/fplinux-icons/APKBUILD",
        ):
            destination = probe_cli.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / relative, destination)
        source = probe_cli.root / ".private/local.c"
        source.parent.mkdir()
        source.write_text("int main(void) { return 0; }\n")
        result = probe_cli.run_probe(".private/local.c", "--output", ".data/tools/local")

        assert (result.returncode) == (1), result.stderr
        assert ("./fplinux setup") in (result.stderr)
        assert not ((probe_cli.root / ".data/tools/local").exists())
        assert not ((probe_cli.root / ".data/kern").exists())

    def test_source_inside_managed_data_is_rejected_before_loading_the_environment(
        self, probe_cli: ProbeCli
    ) -> None:
        """Managed output cannot become the source of a new probe build."""
        source = probe_cli.root / ".data/tools/local.c"
        source.parent.mkdir(parents=True)
        source.write_text("int probe;\n")

        result = probe_cli.run_probe(".data/tools/local.c", "--output", ".data/tools/local")

        assert result.returncode == 1
        assert "probe source must be outside the managed data directory" in result.stderr
        assert not source.with_suffix("").exists()
