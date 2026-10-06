# SPDX-License-Identifier: GPL-2.0-only
"""Maintain the persistent local abuild signing keypair."""

from __future__ import annotations

import os
import pwd
import shutil
import tempfile
from pathlib import Path

from fplinux_cli.build.inputs import CACHE
from fplinux_cli.common import fail, sha256_file

from .aports import _run_as_builder

SIGNING_KEY_DIRECTORY = "apk-signing"
SIGNING_PRIVATE_KEY = "fplinux-build.rsa"
SIGNING_PUBLIC_KEY = "fplinux-build.rsa.pub"


def _ensure_apk_signing_key() -> tuple[Path, Path, str]:
    directory = CACHE / SIGNING_KEY_DIRECTORY
    private_key = directory / SIGNING_PRIVATE_KEY
    public_key = directory / SIGNING_PUBLIC_KEY
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


def signing_public_key(cache: Path) -> Path:
    """Return the persistent local abuild public key path."""
    return cache / SIGNING_KEY_DIRECTORY / SIGNING_PUBLIC_KEY


def signing_key_identity(cache: Path) -> str:
    """Return the SHA-256 identity of the persistent local abuild key."""
    path = signing_public_key(cache)
    if path.is_symlink() or not path.is_file():
        fail(f"package signing public key is missing or invalid: {path}")
    return sha256_file(path)
