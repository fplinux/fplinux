# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: PLR0913, PLR0917
"""Build the selected locked Alpine rootfs and its local APKs."""

from __future__ import annotations

import fcntl
import json
import os
import pwd
import shlex
import shutil
import stat
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO, NoReturn

from . import alpine_state
from .common import ROOT, sha256_file
from .config import relative_value
from .output import current_stage

CACHE = Path("/cache")
SOURCE_DATE_EPOCH = "1784919600"
_ROOTFS_BUILD_LOCK = ".build.lock"


def fail(message: str) -> NoReturn:
    """Stop an Alpine rootfs build without publishing a receipt."""
    raise SystemExit(f"build failed: {message}")


def require_file(path: Path) -> Path:
    """Require one regular, non-symlink file."""
    if path.is_symlink() or not path.is_file():
        fail(f"expected file is missing or invalid: {path}")
    return path


def _ensure_rootfs_directory(cache: Path) -> Path:
    """Create the one real rootfs cache directory without traversing a link."""
    rootfs = cache / "rootfs"
    try:
        metadata = rootfs.lstat()
    except FileNotFoundError:
        try:
            rootfs.mkdir(parents=True)
        except FileExistsError:
            pass
        except OSError as error:
            fail(f"rootfs cache directory cannot be created: {rootfs}: {error}")
        try:
            metadata = rootfs.lstat()
        except OSError as error:
            fail(f"rootfs cache directory is missing or invalid: {rootfs}: {error}")
    except OSError as error:
        fail(f"rootfs cache directory is missing or invalid: {rootfs}: {error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        fail(f"rootfs cache directory is missing or invalid: {rootfs}")
    return rootfs


def _open_rootfs_build_lock(rootfs: Path) -> BinaryIO:
    """Open the one rootfs-build lock without accepting unsafe cache objects."""
    path = rootfs / _ROOTFS_BUILD_LOCK
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        pass
    except OSError as error:
        fail(f"rootfs build lock is missing or invalid: {path}: {error}")
    else:
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            fail(f"rootfs build lock is missing or invalid: {path}")

    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NONBLOCK
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        fail(f"rootfs build lock cannot be opened: {path}: {error}")
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            fail(f"rootfs build lock is missing or invalid: {path}")
        return os.fdopen(descriptor, "r+b")
    except BaseException:
        os.close(descriptor)
        raise


def _require_sha256(value: object, name: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def _build_environment() -> dict[str, str]:
    return {
        **os.environ,
        "LC_ALL": "C",
        "SOURCE_DATE_EPOCH": SOURCE_DATE_EPOCH,
        "KBUILD_BUILD_TIMESTAMP": "2026-07-24 19:00:00 +0000",
        "KBUILD_BUILD_USER": "fplinux",
        "KBUILD_BUILD_HOST": "builder",
        "KBUILD_BUILD_VERSION": "1",
        "KCONFIG_NOTIMESTAMP": "1",
    }


def _run(
    command: list[str],
    *,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
) -> None:
    effective_environment = _build_environment()
    if environment is not None:
        effective_environment.update(environment)
    stage = current_stage()
    if stage is not None:
        stage.run(command, cwd=cwd, env=effective_environment)
        return
    print("+", " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=cwd, env=effective_environment, check=True)


def _log_message(message: str) -> None:
    stage = current_stage()
    if stage is None:
        print(message)
        return
    stage.write((message + "\n").encode())


def _fetch(url: object, expected: object, cache: Path, name: object) -> Path:
    """Fetch one exact HTTPS Alpine artifact into the shared download cache."""
    if not isinstance(url, str) or not url.startswith("https://"):
        fail("source URL must be a non-empty HTTPS URL")
    digest = _require_sha256(expected, f"{name} source")
    relative = relative_value(name, "download cache name")
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_file():
            fail(f"download cache destination is invalid: {destination}")
        if sha256_file(destination) == digest:
            return destination
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- HTTPS is required above.
                url,
                headers={"User-Agent": "FPLinux"},
            )
            with urllib.request.urlopen(  # noqa: S310 -- HTTPS is required above.
                request,
                timeout=60,
            ) as response:
                shutil.copyfileobj(response, output)
        actual = sha256_file(temporary)
        if actual != digest:
            fail(f"{name} SHA256 mismatch: expected {digest}, received {actual}")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def _locked_alpine_artifact(
    lock: dict[str, Any], records: dict[str, dict[str, object]], filename: str
) -> Path:
    record = records.get(filename)
    if record is None:
        fail(f"locked Alpine package is missing: {filename}")
    package = _fetch(
        f"{lock['repository']}/{filename}",
        record.get("sha256"),
        CACHE / "downloads/alpine/packages",
        filename,
    )
    expected_size = record.get("bytes")
    if package.stat().st_size != expected_size:
        fail(
            f"Alpine package size mismatch for {filename}: "
            f"expected {expected_size}, received {package.stat().st_size}"
        )
    return package


def _alpine_group_packages(
    lock: dict[str, Any], records: dict[str, dict[str, object]], group: str
) -> list[Path]:
    return [
        _locked_alpine_artifact(lock, records, filename) for filename in lock[group]["packages"]
    ]


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
    _run(_builder_command(command, environment), cwd=cwd)


def _alpine_source_cache() -> Path:
    cache = CACHE / "downloads/alpine/sources"
    cache.mkdir(parents=True, exist_ok=True)
    _chown_tree(cache, "builder")
    return cache


def _cached_package_files(repository: Path, names: set[str]) -> list[Path] | None:
    packages: list[Path] = []
    for name in sorted(names):
        matches = [path for path in repository.rglob(name) if path.is_file()]
        if len(matches) != 1:
            return None
        packages.append(matches[0])
    return packages


def _apk_package_name(path: Path) -> str:
    """Read the exact package identity carried by one Alpine APK."""
    try:
        with tarfile.open(require_file(path), "r:*") as archive:
            metadata = archive.extractfile(".PKGINFO")
            if metadata is None:
                fail(f"Alpine package has no .PKGINFO: {path}")
            lines = metadata.read().decode("utf-8").splitlines()
    except (OSError, UnicodeDecodeError, tarfile.TarError) as error:
        fail(f"cannot read Alpine package metadata: {path}: {error}")
    names = [line.removeprefix("pkgname = ") for line in lines if line.startswith("pkgname = ")]
    if len(names) != 1 or alpine_state.PACKAGE_ID.fullmatch(names[0]) is None:
        fail(f"Alpine package has an invalid pkgname: {path}")
    return names[0]


def _package_receipt_data(
    repository: Path, recipe: str, package_files: list[Path]
) -> dict[str, object]:
    """Describe the exact signed APK outputs in one aport cache slot."""
    packages: dict[str, dict[str, int | str]] = {}
    for path in sorted(package_files):
        package = _apk_package_name(path)
        if package in packages:
            fail(f"aport produced duplicate package identity: {package}")
        packages[package] = {
            "path": path.relative_to(repository).as_posix(),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    if not packages:
        fail("aport produced no APK packages")
    return {"recipe": _require_sha256(recipe, "Alpine package recipe"), "packages": packages}


def _write_package_receipt(repository: Path, recipe: str, package_files: list[Path]) -> None:
    """Publish a package success receipt only after every APK is in its cache slot."""
    receipt = repository / alpine_state.PACKAGE_RECEIPT_NAME
    encoded = (
        json.dumps(_package_receipt_data(repository, recipe, package_files), sort_keys=True) + "\n"
    ).encode()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=repository, prefix=f".{receipt.name}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(encoded)
        temporary.replace(receipt)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _cached_package_record(
    repository: Path, package: object, record: object
) -> tuple[str, Path] | None:
    """Validate one package entry from a current cache receipt."""
    if (
        not isinstance(package, str)
        or alpine_state.PACKAGE_ID.fullmatch(package) is None
        or not isinstance(record, dict)
        or set(record) != {"path", "sha256", "size"}
    ):
        return None
    relative = record.get("path")
    if not isinstance(relative, str):
        return None
    path = PurePosixPath(relative)
    output = repository / relative
    digest = record.get("sha256")
    size = record.get("size")
    if (
        path.is_absolute()
        or ".." in path.parts
        or path.as_posix() != relative
        or output.is_symlink()
        or not output.is_file()
        or output.suffix != ".apk"
        or not isinstance(size, int)
        or isinstance(size, bool)
        or size < 0
        or output.stat().st_size != size
        or not isinstance(digest, str)
        or sha256_file(output) != digest
        or _apk_package_name(output) != package
    ):
        return None
    return package, output


def _cached_aport_packages(
    name: str, image_recipe: str, signing_key_identity: str
) -> dict[str, Path] | None:
    """Return exact current APK outputs without invoking the cross-build sysroot."""
    recipe = alpine_state.alpine_package_recipe(name, image_recipe, signing_key_identity)
    repository = CACHE / alpine_state.PACKAGE_CACHE_DIRECTORY / name
    try:
        raw = json.loads(
            (repository / alpine_state.PACKAGE_RECEIPT_NAME).read_text(encoding="utf-8")
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(raw, dict) or set(raw) != {"recipe", "packages"}:
        return None
    packages = raw.get("packages")
    if raw.get("recipe") != recipe or not isinstance(packages, dict) or name not in packages:
        return None
    result: dict[str, Path] = {}
    try:
        for package, record in packages.items():
            cached = _cached_package_record(repository, package, record)
            if cached is None:
                return None
            cached_package, output = cached
            result[cached_package] = output
    except SystemExit:
        return None
    return result


def _builder_output(command: list[str], *, cwd: Path, environment: dict[str, str]) -> str:
    result = subprocess.run(
        _builder_command(command, environment),
        cwd=cwd,
        env=_build_environment(),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        fail(f"Alpine package metadata command failed: {detail}")
    return result.stdout


def _ensure_apk_signing_key() -> tuple[Path, Path, str]:
    directory = CACHE / alpine_state.SIGNING_KEY_DIRECTORY
    private_key = directory / alpine_state.SIGNING_PRIVATE_KEY
    public_key = directory / alpine_state.SIGNING_PUBLIC_KEY
    existing = (private_key.exists(), public_key.exists())
    if existing == (True, True):
        if private_key.is_symlink() or not private_key.is_file():
            fail(f"APK signing private key is invalid: {private_key}")
        if public_key.is_symlink() or not public_key.is_file():
            fail(f"APK signing public key is invalid: {public_key}")
        return private_key, public_key, sha256_file(public_key)
    if existing != (False, False):
        fail("APK signing keypair is incomplete; remove /cache/apk-signing and rebuild")

    account = pwd.getpwnam("builder")
    directory.mkdir(mode=0o755, parents=True, exist_ok=True)
    if directory.is_symlink() or not directory.is_dir():
        fail(f"APK signing state directory is invalid: {directory}")
    temporary_home = Path(tempfile.mkdtemp(dir=directory, prefix=".keygen-"))
    os.chown(temporary_home, account.pw_uid, account.pw_gid)
    try:
        _run_as_builder(
            ["abuild-keygen", "-n"],
            cwd=temporary_home,
            environment={"HOME": str(temporary_home)},
        )
        generated_private = sorted((temporary_home / ".abuild").glob("*.rsa"))
        generated_public = sorted((temporary_home / ".abuild").glob("*.rsa.pub"))
        if len(generated_private) != 1 or len(generated_public) != 1:
            fail("abuild-keygen did not create exactly one package keypair")
        shutil.copyfile(generated_private[0], private_key)
        shutil.copyfile(generated_public[0], public_key)
        private_key.chmod(0o600)
        public_key.chmod(0o644)
        os.chown(private_key, account.pw_uid, account.pw_gid)
        os.chown(public_key, account.pw_uid, account.pw_gid)
    finally:
        shutil.rmtree(temporary_home)
    return private_key, public_key, sha256_file(public_key)


def _prepare_alpine_sysroot(
    lock: dict[str, Any], packages: list[Path], sysroot: Path, keys: Path
) -> None:
    _run(
        [
            "apk",
            "--root",
            str(sysroot),
            "--arch",
            lock["arch"],
            "--initdb",
            "--no-network",
            "--no-scripts",
            "--no-logfile",
            "--keys-dir",
            str(keys),
            "add",
            *(str(package) for package in packages),
        ]
    )


def _copy_shared_aport_sources(
    package: str,
    directory: Path,
    *,
    source_root: Path | None = None,
) -> None:
    """Copy one package's declared shared sources into an existing aport stage."""
    if alpine_state.PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
    if source_root is None:
        source_root = ROOT
    if source_root.is_symlink() or not source_root.is_dir():
        fail(f"Alpine source root is missing or invalid: {source_root}")
    if directory.is_symlink() or not directory.is_dir():
        fail(f"staged Alpine aport is missing or invalid: {directory}")
    for shared_source in alpine_state.shared_aport_sources(package, root=source_root):
        source = require_file(shared_source)
        destination = directory / source.name
        if destination.exists() or destination.is_symlink():
            fail(f"shared Alpine source conflicts with aport file: {destination}")
        shutil.copyfile(source, destination)
        destination.chmod(source.stat().st_mode & 0o777)


def materialize_aport_sources(package: str, source_root: Path, destination: Path) -> Path:
    """Copy one canonical aport and its mapped shared files into a writable stage."""
    if alpine_state.PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
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


def _build_fplinux_apks(
    lock: dict[str, Any],
    sysroot: Path,
    work: Path,
    jobs: int,
    private_key: Path,
    public_key: Path,
    build_packages: tuple[str, ...],
) -> tuple[dict[str, Path], Path, Path]:
    if os.geteuid() != 0:
        fail("Alpine package builds require container root; rebuild the current build image")
    aports = work / "aports"
    home = work / "home"
    aports.mkdir()
    for name in build_packages:
        directory = aports / name
        materialize_aport_sources(name, ROOT, directory)
    home.mkdir()
    _chown_tree(aports, "builder")
    _chown_tree(home, "builder")
    sources = _alpine_source_cache()

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
    for name in build_packages:
        directory = aports / name
        recipe = alpine_state.alpine_package_recipe(name, image_recipe, signing_key_identity)
        repository = CACHE / alpine_state.PACKAGE_CACHE_DIRECTORY / name
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
            _log_message(f"Alpine package cache hit: {name} {recipe[:16]}")
            overlap = set(package_outputs) & set(cached)
            if overlap:
                fail(f"abuild package identities are duplicated: {', '.join(sorted(overlap))}")
            package_outputs.update(cached)
            continue

        build_repository = work / "packages" / name
        build_repository.mkdir(parents=True)
        _chown_tree(build_repository, "builder")
        build_environment = {**environment, "REPODEST": str(build_repository)}
        _run_as_builder(["abuild", "-d", "-r"], cwd=directory, environment=build_environment)
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
        outputs = _cached_aport_packages(name, image_recipe, signing_key_identity)
        if outputs is None or {path.name for path in outputs.values()} != listed:
            fail(f"cached abuild receipt differs from listpkg for {name}")
        overlap = set(package_outputs) & set(outputs)
        if overlap:
            fail(f"abuild package identities are duplicated: {', '.join(sorted(overlap))}")
        package_outputs.update(outputs)

    return package_outputs, local_private_key, local_public_key


def _build_alpine_composition_repository(
    lock: dict[str, Any],
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
    _run(
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
    _run(["abuild-sign", "-k", str(private_key), "-p", public_key.name, str(index)])
    return repository, trust


def _alpine_tar_filter(member: tarfile.TarInfo, destination: str) -> tarfile.TarInfo | None:
    if member.issym() or member.islnk():
        target = PurePosixPath(member.linkname)
        if target.is_absolute():
            if ".." in target.parts:
                fail(f"Alpine minirootfs link escapes the root: {member.name}")
            relative_target = target.as_posix().lstrip("/")
            filtered = tarfile.data_filter(member.replace(linkname=relative_target), destination)
            if filtered is None:
                return None
            return filtered.replace(linkname=member.linkname)
    return tarfile.data_filter(member, destination)


def _require_apk_owner(root: Path, path: str, package: str) -> None:
    result = subprocess.run(
        ["apk", "--root", str(root), "--no-network", "info", "-W", path],
        capture_output=True,
        text=True,
        check=False,
        env=_build_environment(),
    )
    expected = f"{path} is owned by {package}-"
    if result.returncode != 0 or not result.stdout.strip().startswith(expected):
        detail = result.stderr.strip() or result.stdout.strip() or "no APK owner reported"
        fail(f"unexpected Alpine package owner for {path}: {detail}")


def _require_bundle_package_absent(root: Path, package: str) -> None:
    result = subprocess.run(
        ["apk", "--root", str(root), "--no-network", "info", "--exists", package],
        capture_output=True,
        text=True,
        check=False,
        env=_build_environment(),
    )
    if result.returncode == 1:
        return
    if result.returncode == 0:
        fail(f"bundle Alpine package was installed in the standard rootfs: {package}")
    detail = result.stderr.strip() or result.stdout.strip() or "no APK diagnostic"
    fail(f"cannot verify that bundle Alpine package is absent: {package}: {detail}")


def _require_openrc_service(root: Path, runlevel: str, service: str) -> None:
    link = root / "etc/runlevels" / runlevel / service
    if not link.is_symlink() or link.readlink() != Path(f"/etc/init.d/{service}"):
        fail(f"Alpine rootfs {runlevel} runlevel is missing {service}")


def _verify_alpine_rootfs(
    root: Path,
    packages: tuple[str, ...],
    bundle_packages: tuple[str, ...] = (),
) -> None:
    init = root / "init"
    if not init.is_symlink() or init.readlink() != Path("/sbin/init"):
        fail("Alpine rootfs /init must point to /sbin/init")

    owners = {
        "/etc/fstab": "fplinux-base",
        "/etc/inittab": "fplinux-base",
        "/etc/os-release": "fplinux-base",
        "/etc/init.d/fplinux-console": "fplinux-console-openrc",
        "/etc/init.d/fplinux-input": "fplinux-input-openrc",
        "/usr/bin/fplinux-console": "fplinux-console",
        "/usr/bin/fplinux-input": "fplinux-input",
    }
    if "fplinux-cpuclock" in packages:
        owners["/usr/bin/fplinux-cpuclock"] = "fplinux-cpuclock"
    if "fplinux-tyrquake" in packages:
        owners["/usr/bin/quake"] = "fplinux-tyrquake"
        owners["/usr/bin/tyr-quake"] = "fplinux-tyrquake"
    if "fplinux-usb-gadget" in packages:
        owners.update(
            {
                "/usr/libexec/fplinux/usb-gadget": "fplinux-usb-gadget",
                "/etc/init.d/fplinux-usb-gadget": "fplinux-usb-gadget-openrc",
                "/etc/init.d/fplinux-usb-dhcp": "fplinux-usb-gadget-openrc",
            }
        )
    if "fplinux-ssh" in packages:
        owners.update(
            {
                "/usr/libexec/fplinux/ssh-server": "fplinux-ssh",
                "/usr/bin/fplinux-session-id": "fplinux-ssh",
                "/etc/init.d/fplinux-ssh": "fplinux-ssh-openrc",
            }
        )
    for path, package in owners.items():
        require_file(root / path.removeprefix("/"))
        _require_apk_owner(root, path, package)

    for service in ("fplinux-input", "fplinux-console"):
        _require_openrc_service(root, "default", service)
    if "fplinux-usb-gadget" in packages:
        _require_openrc_service(root, "sysinit", "fplinux-usb-gadget")
        _require_openrc_service(root, "default", "fplinux-usb-dhcp")
    if "fplinux-ssh" in packages:
        _require_openrc_service(root, "default", "fplinux-ssh")

    fstab = require_file(root / "etc/fstab").read_text(encoding="utf-8")
    if "tmpfs\t/tmp\ttmpfs\trw,nosuid,nodev,mode=1777\t0 0" not in fstab:
        fail("Alpine fstab must mount /tmp as tmpfs")

    world = require_file(root / "etc/apk/world").read_text(encoding="utf-8").splitlines()
    if any("><Q" in entry for entry in world):
        fail("Alpine world must not contain checksum-pinned non-repository packages")
    selected_world = {entry for entry in world if entry.startswith("fplinux-")}
    if selected_world != set(packages):
        fail("Alpine world does not contain the exact selected FPLinux package set")
    for package in bundle_packages:
        _require_bundle_package_absent(root, package)

    inittab = require_file(root / "etc/inittab").read_text(encoding="utf-8")
    if "ttyGS" in inittab or "getty" in inittab:
        fail("Alpine inittab must leave all interactive consoles to OpenRC services")
    for obsolete in (
        "etc/init.d/fplinux-usb-getty",
        "etc/runlevels/default/fplinux-usb-getty",
    ):
        if (root / obsolete).exists() or (root / obsolete).is_symlink():
            fail(f"legacy USB ACM shell is present: /{obsolete}")
    for obsolete in (
        "etc/fplinux-build",
        "usr/libexec/fplinux/init",
        "usr/libexec/fplinux/usb-getty",
    ):
        if (root / obsolete).exists() or (root / obsolete).is_symlink():
            fail(f"obsolete pre-Alpine runtime path is present: /{obsolete}")


def _normalize_rootfs(root: Path) -> None:
    timestamp = int(SOURCE_DATE_EPOCH)
    for path in [*sorted(root.rglob("*"), reverse=True), root]:
        os.utime(path, (timestamp, timestamp), follow_symlinks=False)


def _write_rootfs_cpio(root: Path, destination: Path) -> None:
    command = (
        "find . -xdev -print0 | LC_ALL=C sort -z | "
        "cpio --null --quiet --create --format=newc --reproducible --owner=0:0 "
        f"> {shlex.quote(str(destination))}"
    )
    _run(["/bin/sh", "-c", command], cwd=root)
    require_file(destination)


def _rootfs_install_command(
    lock: dict[str, Any],
    root: Path,
    keys: Path,
    repository: Path,
    packages: tuple[str, ...],
) -> list[str]:
    """Return the exact offline apk composition command for one selected set."""
    return [
        "apk",
        "--root",
        str(root),
        "--arch",
        lock["arch"],
        "--no-network",
        "--no-scripts",
        "--no-logfile",
        "--keys-dir",
        str(keys),
        "--repositories-file",
        "/dev/null",
        "--repository",
        str(repository),
        "add",
        *packages,
    ]


def build_rootfs(
    jobs: int,
    packages: tuple[str, ...],
    bundle_packages: tuple[str, ...] = (),
) -> tuple[Path, Path, str, dict[str, Path]]:
    """Build the standard rootfs and any APKs published in its bundle."""
    image_recipe = os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE", "")
    signing_private_key, signing_public_key, signing_key_identity = _ensure_apk_signing_key()
    recipe = alpine_state.alpine_rootfs_recipe(image_recipe, signing_key_identity, packages)
    overlap = set(packages) & set(bundle_packages)
    if overlap:
        fail(
            "packages cannot be both rootfs-selected and bundle-published: "
            + ", ".join(sorted(overlap))
        )
    build_packages = tuple(sorted((*packages, *bundle_packages)))
    rootfs_directory = _ensure_rootfs_directory(CACHE)
    output = alpine_state.rootfs_output(CACHE, recipe)

    with _open_rootfs_build_lock(rootfs_directory) as lock_stream:
        fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
        rootfs_hit = alpine_state.receipt_matches(output, recipe)
        cached_outputs: dict[str, Path] = {}
        for name in build_packages:
            outputs = _cached_aport_packages(name, image_recipe, signing_key_identity)
            if outputs is None or set(cached_outputs) & set(outputs):
                cached_outputs = {}
                break
            cached_outputs.update(outputs)
        if rootfs_hit and cached_outputs:
            _log_message(f"Alpine rootfs causal receipt hit: {recipe[:16]}")
            return (
                require_file(output / alpine_state.ROOTFS_NAME),
                output,
                recipe,
                {name: cached_outputs[name] for name in bundle_packages},
            )

        lock = alpine_state.load_alpine_lock()
        records = alpine_state.package_records(lock)
        minirootfs_record = lock["minirootfs"]
        minirootfs = _fetch(
            minirootfs_record.get("url"),
            minirootfs_record.get("sha256"),
            CACHE / "downloads/alpine",
            f"alpine-minirootfs-{lock['release']}-{lock['arch']}.tar.gz",
        )
        if minirootfs.stat().st_size != minirootfs_record.get("bytes"):
            fail("locked Alpine minirootfs size does not match its downloaded bytes")
        runtime_packages = _alpine_group_packages(lock, records, "runtime")
        sysroot_packages = _alpine_group_packages(lock, records, "sysroot")

        staging = Path(tempfile.mkdtemp(dir=output.parent, prefix=f".{recipe[:16]}-"))
        staging.chmod(0o755)
        bundle_outputs: dict[str, Path] = {}
        try:
            root = staging / "root"
            sysroot = staging / "sysroot"
            package_work = staging / "package-work"
            root.mkdir()
            sysroot.mkdir()
            package_work.mkdir()
            with tarfile.open(minirootfs, "r:gz") as archive:
                archive.extractall(  # noqa: S202 -- every member passes _alpine_tar_filter.
                    root,
                    filter=_alpine_tar_filter,
                )

            _prepare_alpine_sysroot(lock, sysroot_packages, sysroot, root / "etc/apk/keys")
            local_packages, private_key, public_key = _build_fplinux_apks(
                lock,
                sysroot,
                package_work,
                jobs,
                signing_private_key,
                signing_public_key,
                build_packages,
            )
            try:
                bundle_outputs = {name: local_packages[name] for name in bundle_packages}
            except KeyError as error:
                fail(f"bundle aport did not produce its declared package: {error.args[0]}")
            if rootfs_hit:
                _log_message(f"Alpine rootfs causal receipt hit: {recipe[:16]}")
                return (
                    require_file(output / alpine_state.ROOTFS_NAME),
                    output,
                    recipe,
                    bundle_outputs,
                )
            composition_repository, composition_keys = _build_alpine_composition_repository(
                lock,
                root,
                runtime_packages,
                sorted(local_packages.values()),
                private_key,
                public_key,
                package_work,
            )
            _run(
                _rootfs_install_command(
                    lock,
                    root,
                    composition_keys,
                    composition_repository,
                    packages,
                )
            )

            _verify_alpine_rootfs(root, packages, bundle_packages)
            _normalize_rootfs(root)
            _write_rootfs_cpio(root, staging / alpine_state.ROOTFS_NAME)
            shutil.rmtree(sysroot)
            shutil.rmtree(package_work)
            shutil.rmtree(root)
            alpine_state.write_receipt(staging, recipe)

            if output.exists():
                shutil.rmtree(output)
            staging.replace(output)
            staging = Path()
        finally:
            if staging != Path() and staging.exists():
                shutil.rmtree(staging)

        return (
            require_file(output / alpine_state.ROOTFS_NAME),
            output,
            recipe,
            bundle_outputs,
        )
