# SPDX-License-Identifier: GPL-2.0-only
"""Compose the selected Alpine rootfs under its build lock."""

from __future__ import annotations

import fcntl
import os
import shutil
import stat
import subprocess
import tarfile
import tempfile
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any, BinaryIO

from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.inputs import CACHE, require_file
from fplinux_cli.common import alpine_tar_filter, fail
from fplinux_cli.device_data import inputs as firmware_inputs

from . import lock as alpine_lock
from . import rootfs_state
from .aports import _build_alpine_composition_repository, _build_fplinux_apks
from .packages import (
    _alpine_group_packages,
    _alpine_runtime_packages,
    _cached_aport_packages,
    _prepare_alpine_sysroot,
)
from .recipes import alpine_rootfs_recipe
from .rootfs_files import (
    _install_display_brightness,
    _normalize_rootfs,
    _write_ramroot_initramfs,
    _write_rootfs_cpio,
)
from .rootfs_verify import _require_cached_bundle_packages_installable, _verify_alpine_rootfs
from .selection import aport_build_order
from .signing import _ensure_apk_signing_key

if TYPE_CHECKING:
    from collections.abc import Sequence

_ROOTFS_BUILD_LOCK = ".build.lock"
_alpine_tar_filter = partial(alpine_tar_filter, on_error=fail)


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


def _rootfs_apk_command(
    lock: dict[str, Any],
    root: Path,
    keys: Path,
    repository: Path,
    *arguments: str,
) -> list[str]:
    """Return one offline apk invocation against the composed root."""
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
        *arguments,
    ]


def _rootfs_install_command(
    lock: dict[str, Any],
    root: Path,
    keys: Path,
    repository: Path,
    packages: tuple[str, ...],
) -> list[str]:
    """Return the exact offline apk composition command for one selected set."""
    return _rootfs_apk_command(lock, root, keys, repository, "add", *packages)


def _rootfs_remove_command(
    lock: dict[str, Any],
    root: Path,
    keys: Path,
    repository: Path,
    packages: tuple[str, ...],
) -> list[str]:
    """Return the offline apk command that drops minirootfs packages and their orphans."""
    return _rootfs_apk_command(lock, root, keys, repository, "del", *packages)


def build_rootfs(  # noqa: PLR0913 -- rootfs content and optional image output stay explicit.
    jobs: int,
    packages: tuple[str, ...],
    bundle_packages: tuple[str, ...] = (),
    *,
    firmware: Sequence[firmware_inputs.FirmwareInput] = (),
    display_brightness: dict[str, Any] | None = None,
    external_image: dict[str, Any] | None = None,
    external_output: Path | None = None,
) -> tuple[Path, Path, str, dict[str, Path]]:
    """Build the standard rootfs and any APKs published in its bundle."""
    image_recipe = os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE", "")
    signing_private_key, signing_public_key, signing_key_identity = _ensure_apk_signing_key()
    recipe = alpine_rootfs_recipe(
        image_recipe,
        signing_key_identity,
        packages,
        firmware_inputs=firmware,
        display_brightness=display_brightness,
        root_kind="external" if external_image is not None else "initramfs",
    )
    overlap = set(packages) & set(bundle_packages)
    if overlap:
        fail(
            "packages cannot be both rootfs-selected and bundle-published: "
            + ", ".join(sorted(overlap))
        )
    build_packages = aport_build_order((*packages, *bundle_packages))
    rootfs_directory = _ensure_rootfs_directory(CACHE)
    output = rootfs_state.rootfs_output(CACHE, recipe)
    ext4_root: Any | None = None
    ext4_plan: Any | None = None
    ext4_hit = external_image is None
    if (external_image is None) != (external_output is None):
        fail("external root image and output must be selected together")

    with _open_rootfs_build_lock(rootfs_directory) as lock_stream:
        fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
        rootfs_hit = rootfs_state.receipt_matches(output, recipe)
        if external_image is not None and external_output is not None and rootfs_hit:
            from fplinux_cli.build.storage import ext4 as ext4_root_module  # noqa: PLC0415

            ext4_root = ext4_root_module
            try:
                rootfs_receipt = rootfs_state.trusted_receipt_identity(output, recipe)
                ext4_plan = ext4_root.create_plan(
                    external_image,
                    recipe,
                    rootfs_receipt,
                    image_recipe,
                )
                ext4_hit = ext4_root.cache_hit(external_output, ext4_plan)
            except ext4_root.Ext4RootError as error:
                fail(str(error))
        cached_outputs: dict[str, Path] = {}
        for name in build_packages:
            outputs = _cached_aport_packages(name, image_recipe, signing_key_identity)
            if outputs is None or set(cached_outputs) & set(outputs):
                cached_outputs = {}
                break
            cached_outputs.update(outputs)
        if rootfs_hit and cached_outputs and ext4_hit:
            _require_cached_bundle_packages_installable(
                output / rootfs_state.ROOTFS_NAME,
                tuple(cached_outputs[name] for name in bundle_packages),
            )
            process_build.log_message(f"Alpine rootfs causal receipt hit: {recipe[:16]}")
            return (
                require_file(output / rootfs_state.ROOTFS_NAME),
                output,
                recipe,
                {name: cached_outputs[name] for name in bundle_packages},
            )

        lock = alpine_lock.load_alpine_lock()
        records = alpine_lock.package_records(lock)
        minirootfs_record = lock["minirootfs"]
        minirootfs = sources_build.fetch(
            minirootfs_record.get("url"),
            minirootfs_record.get("sha256"),
            CACHE / "downloads/alpine",
            f"alpine-minirootfs-{lock['release']}-{lock['arch']}.tar.gz",
        )
        if minirootfs.stat().st_size != minirootfs_record.get("bytes"):
            fail("locked Alpine minirootfs size does not match its downloaded bytes")
        runtime_packages = _alpine_runtime_packages(lock, records, packages)
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
                sysroot=sysroot,
                work=package_work,
                jobs=jobs,
                private_key=signing_private_key,
                public_key=signing_public_key,
                build_packages=build_packages,
            )
            try:
                bundle_outputs = {name: local_packages[name] for name in bundle_packages}
            except KeyError as error:
                fail(f"bundle aport did not produce its declared package: {error.args[0]}")
            if rootfs_hit and ext4_hit:
                _require_cached_bundle_packages_installable(
                    output / rootfs_state.ROOTFS_NAME,
                    tuple(bundle_outputs[name] for name in bundle_packages),
                )
                process_build.log_message(f"Alpine rootfs causal receipt hit: {recipe[:16]}")
                return (
                    require_file(output / rootfs_state.ROOTFS_NAME),
                    output,
                    recipe,
                    bundle_outputs,
                )
            composition_repository, composition_keys = _build_alpine_composition_repository(
                lock,
                root=root,
                runtime_packages=runtime_packages,
                local_packages=sorted(local_packages.values()),
                private_key=private_key,
                public_key=public_key,
                work=package_work,
            )
            if "fplinux-apk-tools" in packages:
                # The minirootfs package manager leaves the world first so that
                # its OpenSSL closure is dropped instead of kept beside the
                # Mbed TLS build that the selected set installs.
                process_build.run(
                    _rootfs_remove_command(
                        lock,
                        root,
                        composition_keys,
                        composition_repository,
                        ("apk-tools",),
                    )
                )
            process_build.run(
                _rootfs_install_command(
                    lock,
                    root,
                    composition_keys,
                    composition_repository,
                    packages,
                )
            )

            firmware_inputs.install_firmware_inputs(root, firmware)
            _install_display_brightness(root, display_brightness)
            _verify_alpine_rootfs(
                root,
                packages,
                bundle_packages,
                firmware,
                bundle_apks=tuple(bundle_outputs[name] for name in bundle_packages),
                display_brightness=display_brightness,
            )
            _normalize_rootfs(root)
            if not rootfs_hit:
                _write_rootfs_cpio(root, staging / rootfs_state.ROOTFS_NAME)
                if external_image is None:
                    _write_ramroot_initramfs(root, staging)
                rootfs_state.write_receipt(staging, recipe)
                rootfs_receipt = rootfs_state.trusted_receipt_identity(staging, recipe)
            else:
                rootfs_receipt = rootfs_state.trusted_receipt_identity(output, recipe)
            if external_image is not None and external_output is not None:
                if ext4_root is None:
                    from fplinux_cli.build.storage import ext4 as ext4_root_module  # noqa: PLC0415

                    ext4_root = ext4_root_module
                try:
                    ext4_plan = ext4_root.create_plan(
                        external_image,
                        recipe,
                        rootfs_receipt,
                        image_recipe,
                    )
                    ext4_root.build(root, external_output, ext4_plan)
                except (OSError, subprocess.SubprocessError, ext4_root.Ext4RootError) as error:
                    fail(str(error))
            shutil.rmtree(sysroot)
            shutil.rmtree(package_work)
            shutil.rmtree(root)

            if not rootfs_hit:
                if output.exists():
                    shutil.rmtree(output)
                staging.replace(output)
                staging = Path()
        finally:
            if staging != Path() and staging.exists():
                shutil.rmtree(staging)

        return (
            require_file(output / rootfs_state.ROOTFS_NAME),
            output,
            recipe,
            bundle_outputs,
        )
