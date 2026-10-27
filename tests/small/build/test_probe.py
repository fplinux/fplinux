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
from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.build import probe
from fplinux_cli.environment import kern
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests.source_catalog import acquire_source
from fplinux_cli.reporting import run as output
from fplinux_cli.reporting.run import RunReporter, Stage
from fplinux_cli.workspace import build_inputs as workspace_inputs
from fplinux_cli.workspace import staging as workspace_staging

if TYPE_CHECKING:
    from collections.abc import Sequence


class ProbePublicationTests:
    """Check real file publication; the compiler stub provides no ARM evidence."""

    @staticmethod
    def test_absolute_output_uses_the_selected_external_tools_namespace(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """An external store owns probe outputs without creating it during validation."""
        root = tmp_path / "checkout"
        root.mkdir()
        source = root / "probe.c"
        source.write_bytes(b"int probe;\n")
        external = tmp_path / "external space/данные"
        monkeypatch.setenv("FPLINUX_DATA_DIR", str(external))

        actual = probe.resolve_probe_paths("probe.c", str(external / "tools/probe"), root=root)

        assert actual == (source, external / "tools/probe")
        assert not external.exists()
        with pytest.raises(SystemExit, match="managed tools directory"):
            probe.resolve_probe_paths("probe.c", ".data/tools/probe", root=root)

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
                    "source_id": "alpine_target",
                    "url": "https://example.invalid/minirootfs.tar.gz",
                    "sha256": hashlib.sha256(contents).hexdigest(),
                    "bytes": len(contents),
                },
            }
            (root / "sources.lock.toml").write_text(
                '[alpine_target]\nversion="example"\nname="minirootfs.tar.gz"\n'
                'url="https://example.invalid/minirootfs.tar.gz"\nalgorithm="sha256"\n'
                f'checksum="{hashlib.sha256(contents).hexdigest()}"\nbytes={len(contents)}\n'
            )
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch("urllib.request.urlopen", return_value=io.BytesIO(contents)),
            ):
                acquire_source(root, "alpine_target", root / ".data", offline=False)
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
    @pytest.mark.parametrize("original_present", [False, True], ids=["missing", "saved"])
    def test_first_sysroot_requires_local_base_and_selected_native_packages(
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
        *,
        original_present: bool,
    ) -> None:
        """A probe never downloads; exact local originals permit isolated installation."""
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as bundle:
            key = b"release signing key\n"
            member = tarfile.TarInfo("etc/apk/keys/release.pub")
            member.size = len(key)
            bundle.addfile(member, io.BytesIO(key))
        base = archive.getvalue()
        package = b"selected sysroot package\n"
        (tmp_path / "sources.lock.toml").write_text(
            '[alpine_target]\nversion="fixture"\nname="minirootfs.tar.gz"\n'
            'url="https://example.invalid/minirootfs.tar.gz"\nalgorithm="sha256"\n'
            f'checksum="{hashlib.sha256(base).hexdigest()}"\nbytes={len(base)}\n'
        )
        lock = {
            "arch": "armv7",
            "repositories": {"main": "https://example.invalid/main"},
            "minirootfs": {
                "source_id": "alpine_target",
                "bytes": len(base),
                "url": "https://example.invalid/minirootfs.tar.gz",
                "sha256": hashlib.sha256(base).hexdigest(),
                "destination": "downloads/sources/alpine_target/minirootfs.tar.gz",
            },
            "sysroot": {"packages": ["sysroot.apk"]},
            "runtime": {"packages": ["runtime.apk"], "additions": {}},
            "package": [
                {
                    "file": "sysroot.apk",
                    "repository": "main",
                    "sha256": hashlib.sha256(package).hexdigest(),
                    "bytes": len(package),
                },
                {"file": "runtime.apk", "repository": "main", "sha256": "d" * 64, "bytes": 100},
            ],
        }
        if original_present:
            originals = tmp_path / ".data/downloads"
            base_path = originals / "sources/alpine_target/minirootfs.tar.gz"
            base_path.parent.mkdir(parents=True)
            base_path.write_bytes(base)
            package_path = originals / "alpine/packages/sysroot.apk"
            package_path.parent.mkdir(parents=True)
            package_path.write_bytes(package)
        installed: list[Path] = []

        def install(_stage: Stage, command: list[str], **_kwargs: object) -> None:
            volumes = {
                command[index + 1].split(":")[1]: Path(command[index + 1].split(":")[0])
                for index, argument in enumerate(command[:-1])
                if argument == "--volume"
            }
            assert (volumes["/packages"] / "sysroot.apk").read_bytes() == package
            assert (volumes["/keys"] / "release.pub").read_bytes() == b"release signing key\n"
            assert command[command.index("--network") + 1] == "none"
            assert "--no-network" in command
            (volumes["/sysroot"] / "installed").write_bytes(b"prepared sysroot\n")
            installed.append(volumes["/sysroot"])

        monkeypatch.setattr(common, "ROOT", tmp_path)
        monkeypatch.setattr(output, "ROOT", tmp_path)
        monkeypatch.setattr(alpine_lock, "load_alpine_lock", lambda: lock)
        monkeypatch.setattr(kern, "kern_environment", dict)
        monkeypatch.setattr(Stage, "run", install)
        state = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
        reporter = RunReporter.create("probe-build", target=None, verbose=False)
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            if not original_present:
                with pytest.raises(SystemExit, match=r"minirootfs.tar.gz.*setup --all"):
                    probe._prepare_sysroot("kern", tmp_path / "rootfs", state, reporter)  # noqa: SLF001 -- probe preparation boundary.
                assert installed == []
                assert not list((tmp_path / ".data/probe-build").glob("*/installed"))
                return
            sysroot = probe._prepare_sysroot("kern", tmp_path / "rootfs", state, reporter)  # noqa: SLF001 -- probe preparation boundary.
        assert (sysroot / "installed").read_bytes() == b"prepared sysroot\n"
        assert not (tmp_path / ".data/downloads/alpine/packages/runtime.apk").exists()
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            assert probe._prepare_sysroot("kern", tmp_path / "rootfs", state, reporter) == sysroot  # noqa: SLF001 -- cached preparation boundary.
        reporter.finish()

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
                ".private/probes/probe.c",
                b'#include "../../include/fplinux/probe.h"\nint probe;\n',
                id="explicit-private",
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
            registry = root / ".data/kern/registry"
            registry.parent.mkdir(parents=True)
            registry.write_bytes(b"private runtime state")
            destination = root / ".data/tools/probe"
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
                assert (workspace / sibling.relative_to(root)).read_bytes() == b"#define LOCAL 2\n"
                assert not (workspace / ".data/kern").exists()
                assert not (workspace / ".data/tools/probe").exists()
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
                assert sorted(path.name for path in destination.parent.iterdir()) == ["probe"]
                assert registry.read_bytes() == b"private runtime state"
