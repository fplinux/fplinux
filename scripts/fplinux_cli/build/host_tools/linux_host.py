# SPDX-License-Identifier: GPL-2.0-only
"""Prepare a relocatable Linux x86-64 runtime for standalone archives."""

from __future__ import annotations

import json
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.build import DEFAULT_BUILD_MEMORY_MIB
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.host_tools.host_package import publish_host_package
from fplinux_cli.common import canonical_json_bytes, fail
from fplinux_cli.environment.kern import kern_box_name, kern_environment
from fplinux_cli.environment.runtime import environment_arguments, require_environment
from fplinux_cli.manifests.source_catalog import load_host_sources, resolve_source
from fplinux_cli.templates import substitute_template


def verify_linux_dependencies(files: dict[str, Path]) -> dict[str, list[str]]:
    """Require Linux x86-64 ELF files and a complete bundled shared-library closure."""
    imports: dict[str, list[str]] = {}
    libraries = {
        path.name
        for relative, path in files.items()
        if Path(relative).parent.as_posix() == "runtime/lib"
    }
    for relative, path in sorted(files.items()):
        with path.open("rb") as stream:
            header = stream.read(20)
        if not header.startswith(b"\x7fELF"):
            continue
        if len(header) != 20 or header[4:6] != b"\x02\x01" or header[18:20] != b"\x3e\x00":
            fail(f"Linux runtime file is not a little-endian x86-64 ELF: {relative}")
        result = subprocess.run(
            ["readelf", "-dW", str(path)], capture_output=True, text=True, check=False
        )
        if result.returncode:
            fail(result.stderr.strip() or f"cannot inspect Linux runtime file: {relative}")
        dependencies = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", result.stdout)
        for dependency in dependencies:
            if dependency not in libraries:
                fail(f"Linux runtime library is missing for {relative}: {dependency}")
        imports[relative] = dependencies
    return imports


def _write_launchers(package: Path) -> None:
    directory = package / "tools/linux"
    directory.mkdir(parents=True)
    template = Path(__file__).with_name("linux-launcher.sh.in").read_text(encoding="utf-8")
    for name in ("ssh", "ssh-keygen", "ssh-keyscan"):
        launcher = directory / name
        launcher.write_text(
            substitute_template(template, {"COMMAND": name}),
            encoding="utf-8",
        )
        launcher.chmod(0o755)


def _verify_python(package: Path, *, version: str) -> None:
    runtime = package / "runtime"
    result = subprocess.run(
        [
            str(runtime / "lib/ld-musl-x86_64.so.1"),
            "--library-path",
            str(runtime / "lib"),
            str(runtime / f"bin/python{version.rsplit('.', 1)[0]}"),
            "-B",
            "-I",
            "-S",
            str(Path(__file__).with_name("linux_runtime_probe.py")),
            version,
        ],
        cwd=package.parent,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        fail(result.stderr.strip() or "bundled Linux Python runtime verification failed")


def prepare_linux_host(*, cache: Path, jobs: int) -> dict[str, Path]:
    """Collect runtime files while the caller owns the project cache lock."""
    cache = cache.resolve()
    lock = load_host_sources(common.ROOT, "linux-host.lock.toml")
    originals = {
        name: resolve_source(common.ROOT, entry["source_id"]).acquire(cache, offline=True)
        for name, entry in lock.items()
    }
    python_version = lock["python3"]["version"]
    kern, rootfs, _state = require_environment()
    root = cache / "linux-host"
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root, prefix=".export-") as temporary:
        work = Path(temporary)
        archive = work / "runtime.tar"
        exporter = Path(__file__).with_name("linux_export.py")
        command = [
            kern,
            "box",
            kern_box_name("linux-host-export"),
            *environment_arguments(rootfs),
            "--read-only",
            "--uid-range",
            "--memory",
            f"{DEFAULT_BUILD_MEMORY_MIB}m",
            "--network",
            "none",
            "--tmpfs",
            "/tmp:128m",  # noqa: S108 -- compiler scratch space in the read-only box.
            "--volume",
            f"{exporter}:/export.py:ro",
            "--volume",
            f"{originals['python3']}:/inputs/python.tar.xz:ro",
            "--volume",
            f"{work}:/work",
            "--env",
            f"SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}",
            "--quiet",
            "--",
            "python3",
            "-B",
            "/export.py",
            python_version,
            str(jobs),
        ]
        with archive.open("wb") as output:
            subprocess.run(
                command, cwd=common.ROOT, env=kern_environment(), stdout=output, check=True
            )
        package = work / "package"
        package.mkdir()
        with tarfile.open(archive) as source:
            source.extractall(package, filter="data")
        _write_launchers(package)
        _copy_licenses(package, cache)
        files = {
            path.relative_to(package).as_posix(): path
            for path in package.rglob("*")
            if path.is_file()
        }
        verify_linux_dependencies(files)
        _verify_python(package, version=python_version)
        return publish_host_package(package, root / "package", relative_paths=files)


def _copy_licenses(package: Path, cache: Path) -> None:
    lock = load_host_sources(common.ROOT, "linux-host.lock.toml")
    packages = json.loads((package / "sources/linux-host-packages.json").read_text())
    for component in packages:
        origin = component["origin"]
        if origin not in lock or component["version"] != lock[origin]["version"]:
            fail(
                f"Linux runtime source lock does not match installed package: {component['name']}"
            )
    directory = package / "licenses/linux-host"
    directory.mkdir(parents=True)
    notices = {}
    for name, entry in lock.items():
        source = resolve_source(common.ROOT, entry["source_id"]).acquire(cache, offline=True)
        with tarfile.open(source) as archive:
            for member in entry["licenses"]:
                stream = archive.extractfile(member)
                if stream is not None:
                    (directory / f"{name}-{Path(member).name}").write_bytes(stream.read())
                else:
                    fail(f"Linux runtime license is missing: {member}")
            for member in entry.get("license_headers", []):
                stream = archive.extractfile(member)
                if stream is not None:
                    comment, separator, _ = stream.read().partition(b"*/")
                    if not comment.startswith(b"/*") or not separator:
                        fail(f"Linux runtime license header is missing: {member}")
                    (directory / f"{name}-{Path(member).stem}-LICENSE.txt").write_bytes(
                        comment + b"*/\n"
                    )
                else:
                    fail(f"Linux runtime license is missing: {member}")
        notices[name] = {key: entry[key] for key in ("url", "sha256", "name", "version")}
    (package / "sources/linux-host-downloads.json").write_bytes(canonical_json_bytes(notices))
