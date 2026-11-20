# SPDX-License-Identifier: GPL-2.0-only
"""Produce the prebuilt microSD components installed by the RAM reader."""

from __future__ import annotations

import os
import shutil
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine.rootfs_files import (
    _normalize_rootfs,
    extract_rootfs,
    write_card_rootfs_layer,
)
from fplinux_cli.alpine.rootfs_state import trusted_receipt_identity
from fplinux_cli.alpine.signing import _ensure_apk_signing_key
from fplinux_cli.artifacts.records import write_canonical_json
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.build import process as process_build
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.manifests.values import sha256_value
from fplinux_cli.runtime.card_media import (
    CARD_APK_INDEX,
    CARD_APK_KEY,
    CardMediaError,
    locate_fit_ramdisk,
)

from . import ext4, sd

if TYPE_CHECKING:
    from collections.abc import Mapping

    from fplinux_cli.build.bootstrap.result import ColdSdBootstrap


_DATA_DIRECTORIES = "alpine/card-data-directories"
_SLOT_ROOT = Path("system/a/root")


def _write_package_index(
    rootfs: Path,
    apks: Mapping[str, Path],
    output: Path,
    private_key: Path,
    public_key: Path,
) -> Path:
    """Seal exactly the selected ARM packages for the card's minimal apk."""
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".index.") as name:
        work = Path(name)
        process_build.run(
            [
                "cpio",
                "--extract",
                "--quiet",
                "--make-directories",
                "--no-preserve-owner",
                "--file",
                str(rootfs),
                "--directory",
                str(work),
                "etc/apk/keys/*",
            ]
        )
        keys = work / "etc/apk/keys"
        shutil.copyfile(public_key, keys / public_key.name)
        index = work / "Packages.adb"
        process_build.run(
            [
                "apk",
                "mkndx",
                "--network=no",
                "--keys-dir",
                str(keys),
                "--sign-key",
                str(private_key),
                "--pkgname-spec",
                "${name}.apk",
                "--output",
                str(index),
                *(str(path) for _, path in sorted(apks.items())),
            ]
        )
        index.replace(output)
    return output


def _card_directories(root: Path, path: Path) -> None:
    for component in (*reversed(path.parents), path):
        directory = root / component
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            fail(f"system card directory is invalid: {component}")
        if not directory.exists():
            directory.mkdir()
            directory.chmod(0o755)


def _stage_card_root(archive: Path, partition: Path, directories: Path) -> None:
    """Stage one writable release tree and its shared mount points."""
    slot = partition / _SLOT_ROOT
    _card_directories(partition, _SLOT_ROOT.parent)
    extract_rootfs(archive, slot)
    for line in directories.read_text(encoding="ascii").splitlines():
        relative_name, mode = line.split()
        relative = Path(relative_name)
        _card_directories(slot, relative)
        _card_directories(partition, Path("data") / relative.parent)
        persistent = partition / "data" / relative
        (slot / relative).rename(persistent)
        persistent.chmod(int(mode, 8))
        (slot / relative).mkdir()
        (slot / relative).chmod(int(mode, 8))


def build_partition(  # noqa: PLR0913 -- keep the fit and normalized root as separate inputs.
    target: str,
    spec: dict[str, Any],
    rootfs: Path,
    rootfs_recipe: str,
    output: Path,
    *,
    fit: Path | None,
) -> Path:
    """Build an ext4 partition for a factory card or the installer kit."""
    directories = common.ROOT / _DATA_DIRECTORIES
    selected = {
        **spec,
        "card_layout": {
            "implementation": sha256_file(Path(__file__)),
            "data_directories": sha256_file(directories),
            "factory": {"target": target, "fit": sha256_file(fit)} if fit is not None else None,
        },
    }
    expected_files: tuple[str, ...] = (str(_SLOT_ROOT / "etc/os-release"),)
    if fit is not None:
        expected_files += (
            "system/active",
            "system/target",
            "system/a/FPLINUX.ITB",
            "system/a/base-world",
        )
    plan = ext4.create_plan(
        selected,
        rootfs_recipe,
        trusted_receipt_identity(rootfs.parent, rootfs_recipe),
        sha256_value(os.environ.get("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE"), "environment recipe"),
        expected_files=expected_files,
    )
    if ext4.cache_hit(output, plan):
        return output / str(spec["filename"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".card-tree.") as name:
        partition = Path(name) / "partition"
        partition.mkdir(mode=0o755)
        _stage_card_root(rootfs, partition, directories)
        if fit is not None:
            system = partition / "system"
            shutil.copyfile(fit, system / "a/FPLINUX.ITB")
            (system / "a/FPLINUX.ITB").chmod(0o644)
            world = system / "a/base-world"
            shutil.copyfile(partition / _SLOT_ROOT / "etc/apk/world", world)
            world.chmod(0o644)
            for filename, value in (("active", "a"), ("target", target)):
                record = system / filename
                record.write_text(f"{value}\n", encoding="ascii")
                record.chmod(0o600)
        _normalize_rootfs(partition)
        return ext4.build(partition, output, plan)


def build(  # noqa: PLR0913 -- each selected source and artifact stay explicit.
    target: str,
    target_config: dict[str, Any],
    output: Path,
    *,
    board_rootfs: Path,
    shared_rootfs: Path,
    shared_ext4: Path | None,
    fit: Path | None,
    ramdisk: Path | None,
    cold_boot: ColdSdBootstrap | None,
    bundle_apks: Mapping[str, Path],
) -> dict[str, Path]:
    """Produce the FIT, boot prefix and board layer for the installed system card."""
    if shared_ext4 is None or fit is None or ramdisk is None:
        fail("release card requires a verified ext4 root and FIT")
    if cold_boot is None:
        fail("release card requires a verified autonomous bootstrap")
    output.mkdir(parents=True, exist_ok=True)
    storage = target_config["storage"]
    prefix = output / "boot.xz"
    root = output / "root.ext4.xz"
    board = output / "board.tar"
    layout = output / "layout.json"

    try:
        ramdisk_payload = ramdisk.read_bytes()
        ramdisk_offset, hash_offset = locate_fit_ramdisk(fit.read_bytes(), ramdisk_payload)
        prefix_digest = sd.write_boot_prefix(
            shared_ext4, prefix, storage=storage, cold_boot=cold_boot
        )
        root_digest = sd.compress_card_region(shared_ext4, root, storage["root_size"])
        write_card_rootfs_layer(board_rootfs, shared_rootfs, board)
        private_key, public_key, _identity = _ensure_apk_signing_key()
        index = _write_package_index(
            board_rootfs, bundle_apks, output / CARD_APK_INDEX, private_key, public_key
        )
        write_canonical_json(
            layout,
            {
                "target": target,
                "release_id": current_release_context().release_id,
                "boot_offset": storage["boot_offset"],
                "boot_size": storage["boot_size"],
                "root_offset": storage["root_offset"],
                "root_size": storage["root_size"],
                "disk_signature": storage["disk_signature"],
                "boot_sha256": prefix_digest,
                "root_sha256": root_digest,
                "fit_filename": target_config["fit"]["filename"],
                "fit_ramdisk_offset": ramdisk_offset,
                "fit_ramdisk_size": len(ramdisk_payload),
                "fit_hash_offset": hash_offset,
            },
        )
    except (OSError, CardMediaError, sd.SdImageError) as error:
        fail(str(error))

    return {
        "card/boot.xz": prefix,
        "card/root.ext4.xz": root,
        "card/board.tar": board,
        "card/fit.itb": fit,
        "card/layout.json": layout,
        f"card/{CARD_APK_INDEX}": index,
        f"card/{CARD_APK_KEY}": public_key,
        **{f"card/apks/{name}.apk": path for name, path in bundle_apks.items()},
    }
