# SPDX-License-Identifier: GPL-2.0-only
"""Stage Alpine aports and build their signed package repositories."""

from __future__ import annotations

import os
import pwd
import shlex
import shutil
import subprocess
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.environment import SOURCE_DATE_EPOCH, build_environment
from fplinux_cli.build.inputs import CACHE, require_file
from fplinux_cli.build.process import log_message, run
from fplinux_cli.common import ROOT, fail, sha256_file

from .lock import PACKAGE_ID
from .packages import (
    PACKAGE_CACHE_DIRECTORY,
    _cached_aport_packages,
    _cached_package_files,
    _prepare_alpine_sysroot,
    _write_package_receipt,
)
from .recipes import alpine_package_recipe
from .selection import (
    aport_build_order,
    aport_producer,
    local_build_dependencies,
    shared_aport_sources,
)

if TYPE_CHECKING:
    from collections.abc import Iterator


def _chown_tree(path: Path, user: str) -> None:
    account = pwd.getpwnam(user)
    paths = [path, *sorted(path.rglob("*"))]
    for candidate in paths:
        os.chown(candidate, account.pw_uid, account.pw_gid, follow_symlinks=False)


def _builder_command(command: list[str], environment: dict[str, str]) -> list[str]:
    assignments = [f"{key}={value}" for key, value in sorted(environment.items())]
    shell_command = "exec env " + " ".join(shlex.quote(part) for part in [*assignments, *command])
    return ["su", "builder", "-s", "/bin/sh", "-c", shell_command]


def _run_as_builder(command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
    run(_builder_command(command, environment), cwd=cwd)


@contextmanager
def _alpine_source_cache() -> Iterator[Path]:
    """Lend source ownership for abuild, then return it to the host-mapped root."""
    cache = CACHE / "downloads/alpine/sources"
    cache.mkdir(parents=True, exist_ok=True)
    try:
        _chown_tree(cache, "builder")
        yield cache
    finally:
        _chown_tree(cache, "root")


def _builder_output(command: list[str], *, cwd: Path, environment: dict[str, str]) -> str:
    result = subprocess.run(
        _builder_command(command, environment),
        cwd=cwd,
        env=build_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        fail(f"Alpine package metadata command failed: {detail}")
    return result.stdout


def _copy_shared_aport_sources(
    package: str,
    directory: Path,
    *,
    source_root: Path | None = None,
) -> None:
    """Copy one package's declared shared sources into an existing aport stage."""
    if PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
    if source_root is None:
        source_root = ROOT
    if source_root.is_symlink() or not source_root.is_dir():
        fail(f"Alpine source root is missing or invalid: {source_root}")
    if directory.is_symlink() or not directory.is_dir():
        fail(f"staged Alpine aport is missing or invalid: {directory}")
    for shared_source in shared_aport_sources(package, root=source_root):
        source = require_file(shared_source)
        destination = directory / source.name
        if destination.exists() or destination.is_symlink():
            fail(f"shared Alpine source conflicts with aport file: {destination}")
        shutil.copyfile(source, destination)
        destination.chmod(source.stat().st_mode & 0o777)


def materialize_aport_sources(package: str, source_root: Path, destination: Path) -> Path:
    """Copy one canonical aport and its mapped shared files into a writable stage."""
    if PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
    package = aport_producer(package)
    if source_root.is_symlink() or not source_root.is_dir():
        fail(f"Alpine source root is missing or invalid: {source_root}")
    source_aport = source_root / "alpine/aports" / package
    if source_aport.is_symlink() or not source_aport.is_dir():
        fail(f"canonical Alpine aport is missing or invalid: {source_aport}")
    if destination.exists() or destination.is_symlink():
        fail(f"Alpine aport stage already exists: {destination}")
    for source in [source_aport, *sorted(source_aport.rglob("*"))]:
        if source.is_symlink() or (
            source != source_aport and not source.is_dir() and not source.is_file()
        ):
            fail(f"canonical Alpine aport contains an invalid source: {source}")

    shutil.copytree(source_aport, destination)
    _copy_shared_aport_sources(package, destination, source_root=source_root)
    return destination


def _build_fplinux_apks(  # noqa: PLR0913 -- package, signing and build inputs stay explicit.
    lock: dict[str, Any],
    *,
    sysroot: Path,
    work: Path,
    jobs: int,
    private_key: Path,
    public_key: Path,
    build_packages: tuple[str, ...],
) -> tuple[dict[str, Path], Path, Path]:
    if os.geteuid() != 0:
        fail("Alpine package builds require container root; rebuild the current build image")
    libraries = local_build_dependencies(build_packages)
    build_packages = aport_build_order(build_packages)
    aports = work / "aports"
    home = work / "home"
    aports.mkdir()
    for name in build_packages:
        directory = aports / name
        materialize_aport_sources(name, ROOT, directory)
    home.mkdir()
    _chown_tree(aports, "builder")
    _chown_tree(home, "builder")
    sources = CACHE / "downloads/alpine/sources"

    key_directory = home / ".abuild"
    key_directory.mkdir()
    local_private_key = key_directory / private_key.name
    local_public_key = key_directory / public_key.name
    shutil.copyfile(require_file(private_key), local_private_key)
    shutil.copyfile(require_file(public_key), local_public_key)
    local_private_key.chmod(0o600)
    local_public_key.chmod(0o644)
    project_config = (ROOT / "alpine/abuild.conf").read_text(encoding="utf-8")
    generated_config = key_directory / "abuild.conf"
    generated_config.write_text(
        f'PACKAGER_PRIVKEY="{local_private_key}"\n' + project_config,
        encoding="utf-8",
    )
    _chown_tree(key_directory, "builder")

    environment = {
        "HOME": str(home),
        "APK": f"apk --keys-dir {home / '.abuild'}",
        "CBUILD": "x86_64-alpine-linux-musl",
        "CHOST": lock["triplet"],
        "CTARGET": lock["triplet"],
        "CBUILDROOT": str(sysroot),
        "BOOTSTRAP": "no",
        "SRCDEST": str(sources),
        "SOURCE_DATE_EPOCH": SOURCE_DATE_EPOCH,
        "JOBS": str(jobs),
        "MAKEFLAGS": f"-j{jobs}",
    }
    expected_filenames: set[str] = set()
    package_outputs: dict[str, Path] = {}
    image_recipe = os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE", "")
    signing_key_identity = sha256_file(public_key)
    with ExitStack() as build_state:
        sources_borrowed = False
        for name in build_packages:
            directory = aports / name
            recipe = alpine_package_recipe(name, image_recipe, signing_key_identity)
            repository = CACHE / PACKAGE_CACHE_DIRECTORY / name
            package_environment = {**environment, "REPODEST": str(repository)}
            require_file(directory / "APKBUILD")
            _run_as_builder(
                ["apkbuild-lint", "APKBUILD"], cwd=directory, environment=package_environment
            )
            listed = {
                line.strip()
                for line in _builder_output(
                    ["abuild", "listpkg"], cwd=directory, environment=package_environment
                ).splitlines()
                if line.strip()
            }
            if not listed or any(
                Path(package).name != package or not package.endswith(".apk") for package in listed
            ):
                fail(f"abuild listpkg returned invalid package names for {name}")
            duplicate = expected_filenames & listed
            if duplicate:
                fail(f"abuild package names are duplicated: {', '.join(sorted(duplicate))}")
            expected_filenames.update(listed)
            cached = _cached_aport_packages(name, image_recipe, signing_key_identity)
            if cached is not None and {path.name for path in cached.values()} == listed:
                log_message(f"Alpine package cache hit: {name} {recipe[:16]}")
                outputs = cached
            else:
                if not sources_borrowed:
                    build_state.enter_context(_alpine_source_cache())
                    sources_borrowed = True
                build_repository = work / "packages" / name
                build_repository.mkdir(parents=True)
                _chown_tree(build_repository, "builder")
                build_environment = {**environment, "REPODEST": str(build_repository)}
                _run_as_builder(
                    ["abuild", "-d", "-r"], cwd=directory, environment=build_environment
                )
                built = _cached_package_files(build_repository, listed)
                if built is None:
                    fail(f"abuild repository output differs from listpkg for {name}")
                if repository.exists():
                    shutil.rmtree(repository)
                shutil.copytree(build_repository, repository)
                cached_files = _cached_package_files(repository, listed)
                if cached_files is None:
                    fail(f"cached abuild output differs from listpkg for {name}")
                _write_package_receipt(repository, recipe, cached_files)
                built_outputs = _cached_aport_packages(name, image_recipe, signing_key_identity)
                if (
                    built_outputs is None
                    or {path.name for path in built_outputs.values()} != listed
                ):
                    fail(f"cached abuild receipt differs from listpkg for {name}")
                outputs = built_outputs
            overlap = set(package_outputs) & set(outputs)
            if overlap:
                fail(f"abuild package identities are duplicated: {', '.join(sorted(overlap))}")
            package_outputs.update(outputs)
            if name in libraries:
                _prepare_alpine_sysroot(lock, sorted(outputs.values()), sysroot, key_directory)

    return package_outputs, local_private_key, local_public_key


def _build_alpine_composition_repository(  # noqa: PLR0913 -- package and signing inputs stay explicit.
    lock: dict[str, Any],
    *,
    root: Path,
    runtime_packages: list[Path],
    local_packages: list[Path],
    private_key: Path,
    public_key: Path,
    work: Path,
) -> tuple[Path, Path]:
    repository = work / "composition-repository"
    package_directory = repository / lock["arch"]
    trust = work / "composition-keys"
    package_directory.mkdir(parents=True)
    trust.mkdir()

    for key in sorted((root / "etc/apk/keys").glob("*.rsa.pub")):
        shutil.copyfile(key, trust / key.name)
    if not any(trust.iterdir()):
        fail("Alpine minirootfs contains no trusted package keys")
    shutil.copyfile(public_key, trust / public_key.name)

    copied: list[Path] = []
    for package in [*runtime_packages, *local_packages]:
        destination = package_directory / package.name
        if destination.exists():
            fail(f"composition repository package is duplicated: {package.name}")
        shutil.copyfile(require_file(package), destination)
        copied.append(destination)

    index = package_directory / "APKINDEX.tar.gz"
    run(
        [
            "apk",
            "index",
            "--keys-dir",
            str(trust),
            "--no-warnings",
            "--rewrite-arch",
            lock["arch"],
            "--description",
            "FPLinux locked runtime repository",
            "--output",
            str(index),
            *(str(package) for package in copied),
        ]
    )
    run(["abuild-sign", "-k", str(private_key), "-p", public_key.name, str(index)])
    return repository, trust
