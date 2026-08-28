# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- validation failures use exact artifact diagnostics.
"""Build one deterministic ext4 filesystem from a verified Alpine root tree."""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import artifact_state, build_env
from .artifact_state import (
    canonical_json_bytes,
    receipt_matches,
    regular_file_record,
    require_lowercase_sha256,
    write_canonical_json,
)
from .common import sha256_file

RECEIPT_NAME = ".fplinux-ext4-receipt.json"
SUPERBLOCK_OFFSET = 1024
EXT4_MAGIC = 0xEF53
FEATURES = (
    "none,sparse_super,large_file,filetype,dir_index,ext_attr,has_journal,"
    "extent,huge_file,flex_bg,metadata_csum,metadata_csum_seed,64bit,"
    "dir_nlink,extra_isize,orphan_file"
)
_COMMAND_TIMEOUT_SECONDS = 300


class Ext4RootError(RuntimeError):
    """The ext4 recipe, input tree, output or receipt is invalid."""


@dataclass(frozen=True)
class Ext4Plan:
    """Exact inputs whose image output may be reused."""

    recipe: str
    spec: dict[str, Any]
    rootfs_receipt: dict[str, str]


def create_plan(
    spec: dict[str, Any],
    rootfs_recipe: str,
    rootfs_receipt: dict[str, str],
    container_recipe: str,
) -> Ext4Plan:
    """Create the causal recipe for one profile-owned ext4 artifact."""
    if spec.get("kind") != "ext4-root":
        raise Ext4RootError("ext4 image kind is invalid")
    if set(rootfs_receipt) != {"recipe", "sha256"}:
        raise Ext4RootError("rootfs receipt identity is invalid")
    receipt = {
        "recipe": require_lowercase_sha256(
            rootfs_receipt.get("recipe"), "rootfs recipe", Ext4RootError
        ),
        "sha256": require_lowercase_sha256(
            rootfs_receipt.get("sha256"), "rootfs receipt SHA-256", Ext4RootError
        ),
    }
    if receipt["recipe"] != require_lowercase_sha256(
        rootfs_recipe, "rootfs recipe", Ext4RootError
    ):
        raise Ext4RootError("rootfs receipt does not match its recipe")
    config = Path(__file__).with_name("mke2fs.conf")
    implementation = Path(__file__)
    manifest = {
        "spec": spec,
        "rootfs_receipt": receipt,
        "container_image_recipe": require_lowercase_sha256(
            container_recipe, "container image recipe", Ext4RootError
        ),
        "source_date_epoch": build_env.SOURCE_DATE_EPOCH,
        "features": FEATURES,
        "implementation": {
            "builder": regular_file_record(implementation, "ext4 file", Ext4RootError),
            "artifact_state": regular_file_record(
                Path(artifact_state.__file__), "ext4 file", Ext4RootError
            ),
            "build_env": regular_file_record(Path(build_env.__file__), "ext4 file", Ext4RootError),
            "mke2fs_config": regular_file_record(config, "ext4 file", Ext4RootError),
        },
    }
    recipe = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    return Ext4Plan(recipe, dict(spec), receipt)


def _receipt_payload(plan: Ext4Plan, output: Path) -> dict[str, object]:
    image = output / str(plan.spec["filename"])
    return {
        "recipe": plan.recipe,
        "rootfs_receipt": plan.rootfs_receipt,
        "image": regular_file_record(image, "ext4 file", Ext4RootError),
    }


def cache_hit(output: Path, plan: Ext4Plan) -> bool:
    """Return true only for the exact complete ext4 artifact and receipt."""
    try:
        expected = _receipt_payload(plan, output)
    except Ext4RootError:
        return False
    return receipt_matches(output / RECEIPT_NAME, expected)


def receipt_identity(output: Path, plan: Ext4Plan) -> dict[str, str]:
    """Return the identity of one fully rechecked ext4 receipt."""
    if not cache_hit(output, plan):
        raise Ext4RootError("ext4 receipt is missing, stale or invalid")
    return {"recipe": plan.recipe, "sha256": sha256_file(output / RECEIPT_NAME)}


def _reject_semantic_xattrs(root: Path) -> None:
    """Do not silently drop package-owned capabilities, ACLs or user metadata."""
    for path in (root, *sorted(root.rglob("*"))):
        try:
            attributes = os.listxattr(path, follow_symlinks=False)
        except OSError as error:
            raise Ext4RootError(f"cannot inspect rootfs xattrs: {path}: {error}") from error
        semantic = [name for name in attributes if name != "security.selinux"]
        if semantic:
            raise Ext4RootError(
                f"rootfs contains xattrs not represented by the current image contract: {path}"
            )


def _run(command: list[str], *, environment: dict[str, str] | None = None) -> None:
    effective_environment = build_env.build_environment()
    if environment is not None:
        effective_environment.update(environment)
    subprocess.run(
        command,
        env=effective_environment,
        check=True,
        timeout=_COMMAND_TIMEOUT_SECONDS,
    )


def _verify_superblock(image: Path, spec: dict[str, Any]) -> None:
    with image.open("rb") as stream:
        stream.seek(SUPERBLOCK_OFFSET)
        superblock = stream.read(1024)
    if len(superblock) != 1024 or struct.unpack_from("<H", superblock, 56)[0] != EXT4_MAGIC:
        raise Ext4RootError("ext4 image has an invalid superblock")
    block_size = 1024 << struct.unpack_from("<I", superblock, 24)[0]
    inode_size = struct.unpack_from("<H", superblock, 88)[0]
    filesystem_uuid = uuid.UUID(bytes=superblock[104:120])
    label = superblock[120:136].split(b"\0", 1)[0].decode("ascii")
    if block_size != spec["block_size"]:
        raise Ext4RootError("ext4 block size differs from its profile")
    if inode_size != spec["inode_size"]:
        raise Ext4RootError("ext4 inode size differs from its profile")
    if str(filesystem_uuid) != spec["uuid"]:
        raise Ext4RootError("ext4 UUID differs from its profile")
    if label != spec["label"]:
        raise Ext4RootError("ext4 label differs from its profile")


def _verify_contents(image: Path, root: Path, temporary: Path) -> None:
    dumped = temporary / "os-release"
    result = subprocess.run(
        ["debugfs", "-R", f"dump /etc/os-release {dumped}", str(image)],
        capture_output=True,
        text=True,
        env=build_env.build_environment(),
        check=False,
        timeout=_COMMAND_TIMEOUT_SECONDS,
    )
    if result.returncode != 0 or not dumped.is_file():
        detail = result.stderr.strip() or result.stdout.strip()
        raise Ext4RootError(detail or "debugfs could not read /etc/os-release")
    if dumped.read_bytes() != (root / "etc/os-release").read_bytes():
        raise Ext4RootError("ext4 /etc/os-release differs from the normalized root tree")


def _verify_image(image: Path, root: Path, spec: dict[str, Any], temporary: Path) -> None:
    if image.stat().st_size != spec["size"]:
        raise Ext4RootError("ext4 image size differs from its profile")
    _verify_superblock(image, spec)
    _run(["e2fsck", "-f", "-n", str(image)])
    _verify_contents(image, root, temporary)


def build(root: Path, output: Path, plan: Ext4Plan) -> Path:
    """Build and atomically publish one deterministic ext4 image."""
    if root.is_symlink() or not root.is_dir():
        raise Ext4RootError("normalized root tree is missing or invalid")
    _reject_semantic_xattrs(root)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".ext4-root.") as name:
        temporary = Path(name)
        staging = temporary / "publish"
        staging.mkdir()
        image = staging / str(plan.spec["filename"])
        with image.open("wb") as stream:
            stream.truncate(int(plan.spec["size"]))
        image.chmod(0o644)
        config = Path(__file__).with_name("mke2fs.conf")
        _run(
            [
                "mke2fs",
                "-q",
                "-F",
                "-t",
                "ext4",
                "-T",
                "default",
                "-o",
                "linux",
                "-b",
                str(plan.spec["block_size"]),
                "-I",
                str(plan.spec["inode_size"]),
                "-i",
                "16384",
                "-m",
                "0",
                "-L",
                str(plan.spec["label"]),
                "-M",
                "/",
                "-U",
                str(plan.spec["uuid"]),
                "-e",
                "remount-ro",
                "-O",
                FEATURES,
                "-E",
                (
                    "lazy_itable_init=0,lazy_journal_init=0,nodiscard,no_copy_xattrs,"
                    f"root_owner=0:0,root_perms=0755,hash_seed={plan.spec['uuid']}"
                ),
                "-d",
                str(root),
                str(image),
            ],
            environment={
                "MKE2FS_CONFIG": str(config),
                "MKE2FS_DEVICE_SECTSIZE": "512",
                "MKE2FS_DEVICE_PHYS_SECTSIZE": "512",
            },
        )
        _verify_image(image, root, plan.spec, temporary)
        write_canonical_json(staging / RECEIPT_NAME, _receipt_payload(plan, staging), mode=0o644)
        if output.exists():
            if output.is_symlink() or not output.is_dir():
                raise Ext4RootError(f"ext4 output is invalid: {output}")
            shutil.rmtree(output)
        staging.replace(output)
    if not cache_hit(output, plan):
        raise Ext4RootError("published ext4 receipt is not reusable")
    return output / str(plan.spec["filename"])
