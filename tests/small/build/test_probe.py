# SPDX-License-Identifier: GPL-2.0-only
"""Probe input isolation and publication with an explicit compiler-process stub."""

from __future__ import annotations

import hashlib
import io
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import probe
from fplinux_cli.environment import kern
from fplinux_cli.reporting import run as output
from fplinux_cli.reporting.run import RunReporter, Stage
from fplinux_cli.workspace import build_inputs as workspace_inputs
from fplinux_cli.workspace import staging as workspace_staging

if TYPE_CHECKING:
    from collections.abc import Sequence


class ProbePublicationTests:
    """Check real file publication; the compiler stub provides no ARM evidence."""

    @staticmethod
    def test_preparation_extracts_release_keys_from_verified_archive() -> None:
        """Probe preparation reuses verified minirootfs bytes and extracts only signing keys."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = io.BytesIO()
            with tarfile.open(fileobj=archive, mode="w:gz") as bundle:
                for name, contents in (
                    ("etc/apk/keys/release.pub", b"release signing key\n"),
                    ("etc/irrelevant", b"not a key\n"),
                ):
                    member = tarfile.TarInfo(name)
                    member.size = len(contents)
                    bundle.addfile(member, io.BytesIO(contents))
            contents = archive.getvalue()
            lock = {
                "release": "example",
                "arch": "armv7",
                "minirootfs": {
                    "url": "https://example.invalid/minirootfs.tar.gz",
                    "sha256": hashlib.sha256(contents).hexdigest(),
                    "bytes": len(contents),
                },
            }
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch("urllib.request.urlopen", return_value=io.BytesIO(contents)),
            ):
                keys = probe._prepare_keys(lock, root / "first")  # noqa: SLF001
            assert ((keys / "release.pub").read_bytes()) == (b"release signing key\n")
            assert not ((root / "first/etc/irrelevant").exists())
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                keys = probe._prepare_keys(lock, root / "second")  # noqa: SLF001
            assert ((keys / "release.pub").read_bytes()) == (b"release signing key\n")

    @staticmethod
    @pytest.mark.parametrize(
        "succeeds",
        [pytest.param(False, id="compiler-failure"), pytest.param(True, id="compiler-success")],
    )
    @pytest.mark.parametrize(
        ("source_name", "source_contents"),
        [
            pytest.param(
                "tools/probes/probe.c",
                b'#include "../../include/fplinux/probe.h"\n#include "local.h"\nint probe;\n',
                id="ordinary",
            ),
            pytest.param(
                ".cache/tools/probe.c",
                b'#include "../../include/fplinux/probe.h"\nint probe;\n',
                id="ignored",
            ),
        ],
    )
    def test_compilation_uses_staged_inputs_and_publishes_only_on_success(
        source_name: str, source_contents: bytes, *, succeeds: bool
    ) -> None:
        """Compile isolated source bytes, retaining an earlier output on compiler failure."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / source_name
            source.parent.mkdir(parents=True)
            source.write_bytes(source_contents)
            header = root / "include/fplinux/probe.h"
            header.parent.mkdir(parents=True)
            header.write_bytes(b'#include "probe-detail.h"\n#define VALUE 0\n')
            (header.parent / "probe-detail.h").write_bytes(b"#define DETAIL 1\n")
            sibling = source.with_name("local.h")
            sibling.write_bytes(b"#define LOCAL 2\n")
            registry = root / ".cache/kern/registry"
            registry.parent.mkdir(parents=True)
            registry.write_bytes(b"private runtime state")
            destination = root / ".cache/tools/probe"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(b"previous executable")
            destination.chmod(0o751)
            staged_workspaces: list[Path] = []

            def compiler_stub(command: Sequence[str], **_kwargs: object) -> None:
                """Observe the mounted inputs before returning controlled compiler output."""
                mount = next(
                    argument for argument in command if argument.endswith(":/workspace:ro")
                )
                workspace = Path(mount.removesuffix(":/workspace:ro"))
                staged_workspaces.append(workspace)
                assert not registry.is_relative_to(workspace)
                source.write_bytes(b"changed checkout source\n")
                header.write_bytes(b"changed checkout header\n")
                sibling.write_bytes(b"changed checkout sibling\n")
                assert (workspace / source_name).read_bytes() == source_contents
                assert (workspace / "include/fplinux/probe.h").read_bytes() == (
                    b'#include "probe-detail.h"\n#define VALUE 0\n'
                )
                assert (workspace / "include/fplinux/probe-detail.h").read_bytes() == (
                    b"#define DETAIL 1\n"
                )
                if source_name.startswith(".cache/"):
                    assert not (workspace / sibling.relative_to(root)).exists()
                else:
                    assert (workspace / sibling.relative_to(root)).read_bytes() == (
                        b"#define LOCAL 2\n"
                    )
                assert not (workspace / ".cache/kern").exists()
                assert not (workspace / ".cache/tools/probe").exists()
                assert f"/workspace/{source_name}" in command
                requested = Path(command[command.index("-o") + 1])
                (destination.parent / requested.name).write_bytes(b"new executable")
                if not succeeds:
                    raise SystemExit(1)

            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(kern, "ROOT", root),
                mock.patch.object(workspace_inputs, "ROOT", root),
                mock.patch.object(workspace_staging, "ROOT", root),
                mock.patch.object(Stage, "run", side_effect=compiler_stub),
            ):
                reporter = RunReporter.create("probe-build", target=None, verbose=False)

                def compile_probe(run: RunReporter) -> None:
                    probe._compile_probe(  # noqa: SLF001 -- publication boundary.
                        source,
                        destination,
                        root / "sysroot",
                        kern="stubbed-kern",
                        rootfs=root / "environment/rootfs",
                        reporter=run,
                    )

                if succeeds:
                    compile_probe(reporter)
                    assert destination.read_bytes() == b"new executable"
                    assert destination.stat().st_mode & 0o777 == 0o755
                else:
                    with pytest.raises(SystemExit):
                        compile_probe(reporter)
                    assert destination.read_bytes() == b"previous executable"
                    assert destination.stat().st_mode & 0o777 == 0o751
                assert staged_workspaces
                assert all(not workspace.exists() for workspace in staged_workspaces)
                assert sorted(path.name for path in destination.parent.iterdir()) == (
                    ["local.h", "probe", "probe.c"]
                    if source_name.startswith(".cache/")
                    else ["probe"]
                )
                assert registry.read_bytes() == b"private runtime state"
