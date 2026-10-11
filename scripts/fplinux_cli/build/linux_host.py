# SPDX-License-Identifier: GPL-2.0-only
"""Prepare a relocatable Linux x86-64 runtime for standalone archives."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, sha256_file
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.environment.image_store import current_image_state
from fplinux_cli.environment.images import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.environment.kern import kern_box_name, kern_environment, require_kern


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
    for name in ("ssh", "ssh-keygen", "ssh-keyscan"):
        launcher = directory / name
        launcher.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            'command_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            'runtime_root=$(CDPATH= cd -- "$command_root/../../runtime" && pwd)\n'
            'exec "$runtime_root/lib/ld-musl-x86_64.so.1" '
            '--library-path "$runtime_root/lib" '
            f'"$runtime_root/bin/{name}" "$@"\n',
            encoding="utf-8",
        )
        launcher.chmod(0o755)


def _verify_python(package: Path) -> None:
    runtime = package / "runtime"
    result = subprocess.run(
        [
            str(runtime / "lib/ld-musl-x86_64.so.1"),
            "--library-path",
            str(runtime / "lib"),
            str(runtime / "bin/python3.14"),
            "-B",
            "-I",
            "-S",
            "-c",
            (
                "import ctypes,math,cmath,lzma,tomllib,hashlib,json,sys; "
                "assert sys.version_info[:2] == (3,14); "
                "assert hashlib.sha256(b'test').hexdigest() == "
                "'9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08'; "
                "assert lzma.decompress(lzma.compress(b'test')) == b'test'; "
                "assert tomllib.loads('value=42')['value'] == 42; "
                "assert math.sqrt(81) == 9; assert cmath.sqrt(-1) == 1j; "
                "assert ctypes.sizeof(ctypes.c_uint64) == 8"
            ),
        ],
        cwd=package.parent,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        fail(result.stderr.strip() or "bundled Linux Python runtime verification failed")


def prepare_linux_host(*, cache: Path, offline: bool) -> dict[str, Path]:
    """Collect runtime files while the caller owns the project cache lock."""
    cache = cache.resolve()
    lock = load_container_lock()
    recipe = container_image_recipe_digest(lock)
    image = container_image_reference(lock, recipe)
    kern = require_kern(lock)
    state = current_image_state(kern, image, recipe)
    if state is None:
        fail(
            "Linux standalone runtime requires the current pinned image; run ./fplinux setup first"
        )
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
            "--image",
            image,
            "--pull",
            "never",
            "--read-only",
            "--network",
            "none",
            "--volume",
            f"{exporter}:/export.py:ro",
            "--quiet",
            "--",
            "python3",
            "-B",
            "/export.py",
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
        _copy_licenses(package, root / "downloads", offline=offline)
        files = {
            path.relative_to(package).as_posix(): path
            for path in package.rglob("*")
            if path.is_file()
        }
        imports = verify_linux_dependencies(files)
        _verify_python(package)
        destination = root / "package"
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(package, destination)
        result = {relative: destination / relative for relative in files}
        receipt = {
            "image_content": state.image_content,
            "files": {relative: sha256_file(path) for relative, path in sorted(result.items())},
            "imports": imports,
        }
        (root / "receipt.json").write_bytes(canonical_json_bytes(receipt))
        return result


def _copy_licenses(package: Path, downloads: Path, *, offline: bool) -> None:
    lock = common.load_toml(common.ROOT / "linux-host.lock.toml")
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
        source = download_locked_file(
            entry["url"], entry["sha256"], downloads / entry["name"], offline=offline
        )
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
